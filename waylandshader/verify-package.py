#!/usr/bin/env python3
"""Inspect the fork's Arch package without extracting or executing its payload."""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import sys
import tarfile


PACKAGE = "niri-waylandshader"
SERVICE = f"usr/lib/systemd/user/{PACKAGE}.service"
SHUTDOWN = f"usr/lib/systemd/user/{PACKAGE}-shutdown.target"
DESKTOP = f"usr/share/wayland-sessions/{PACKAGE}.desktop"
EXECUTABLES = {
    f"usr/bin/{PACKAGE}", f"usr/bin/{PACKAGE}-session",
    "usr/bin/waylandshader-niri-controller", "usr/bin/waylandshader-nirictl",
}
REQUIRED = EXECUTABLES | {
    ".PKGINFO", SERVICE, SHUTDOWN, DESKTOP,
    "usr/lib/waylandshader/libwaylandshader-rashader.so.2",
    f"usr/lib/dinit.d/user/{PACKAGE}", f"usr/lib/dinit.d/user/{PACKAGE}.target",
    "usr/share/applications/org.waylandshader.NiriController.desktop",
} | {
    f"usr/share/doc/{PACKAGE}/{name}.md"
    for name in ("README", "REFACTORING", "HISTORY", "UPGRADING")
} | {
    f"usr/share/licenses/{PACKAGE}/{name}"
    for name in ("niri-GPL-3.0", "waylandshader-MIT", "librashader-MPL-2.0")
}
STOCK_PATHS = {
    "usr/bin/niri", "usr/bin/niri-session", "usr/share/wayland-sessions/niri.desktop",
    "usr/lib/systemd/user/niri.service", "usr/lib/systemd/user/niri-shutdown.target",
    "usr/lib/dinit.d/user/niri", "usr/lib/dinit.d/user/niri.target",
}
MAX_ARCHIVE = 512 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
MAX_TEXT = 64 * 1024
MAX_MEMBERS = 4096
LIMITS = {
    "archive_bytes": MAX_ARCHIVE,
    "expanded_bytes": MAX_EXPANDED,
    "text_or_extended_header_bytes": MAX_TEXT,
    "members": MAX_MEMBERS,
    "zstd_window_bytes": 64 * 1024 * 1024,
    "compression": "uncompressed tar; zstd requires Python 3.14+ compression.zstd",
    "member_types": "regular files and directories only; no links, sparse files or special files",
    "systemd_assets": "fork compositor service and shutdown target only; no extra units or drop-ins",
    "not_checked": [
        "signatures, authenticity, provenance or .MTREE contents",
        "ELF architecture/ABI, shared-library resolution or runtime behavior",
        "shell/dinit execution semantics or full systemd/desktop syntax validation",
        "source availability or license compliance (notice presence only)",
    ],
}


class BoundedReader:
    """Forward-only tar input, bounding header allocations and expanded bytes."""

    def __init__(self, source):
        self.source = source
        self.position = 0

    def tell(self):
        return self.position

    def read(self, size):
        if size < 0 or size > MAX_TEXT:
            raise ValueError("tar read exceeds the text/extended-header limit")
        data = self.source.read(size)
        self.position += len(data)
        if self.position > MAX_EXPANDED:
            raise ValueError("archive exceeds the expanded-byte limit")
        return data

    def seek(self, offset, whence=0):
        if whence != 0 or offset < self.position:
            raise ValueError("archive requires unsupported backward seeking")
        if offset > MAX_EXPANDED:
            raise ValueError("archive exceeds the expanded-byte limit")
        while self.position < offset:
            if not self.read(min(MAX_TEXT, offset - self.position)):
                raise ValueError("truncated archive payload")
        return self.position




def canonical_name(name, directory=False):
    if directory:
        # Match tarfile's directory normalization before collision checks.
        name = name.rstrip("/")
    if (not name or len(name) > 1024 or "\\" in name
            or any(ord(char) < 32 or ord(char) == 127 for char in name)
            or any(part in ("", ".", "..") for part in name.split("/"))):
        raise ValueError(f"unsafe or noncanonical archive name: {name!r}")
    return name


def assignments(text, name):
    """Keep repeated assignments and systemd's empty-assignment list resets."""
    result = {}
    section = ""
    pending = ""
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        pending += line
        if pending.endswith("\\"):
            pending = pending[:-1] + " "
            continue
        line, pending = pending, ""
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        else:
            key, separator, value = line.partition("=")
            if not separator or not key.strip():
                raise ValueError(f"malformed assignment in {name}")
            values = result.setdefault((section, key.strip()), [])
            if value.strip():
                values.append(value.strip())
            else:
                values.clear()
    if pending:
        raise ValueError(f"unterminated continuation in {name}")
    return result


def check_contract(members, texts, report):
    errors = report["errors"]
    for name in sorted(REQUIRED):
        member = members.get(name)
        if member is None or not member.isfile() or member.size == 0:
            errors.append(f"missing or empty required regular file: {name}")
        elif name in EXECUTABLES and member.mode & 0o111 != 0o111:
            errors.append(f"required executable is not executable for all users: {name}")

    if ".PKGINFO" in texts:
        metadata = assignments(texts[".PKGINFO"], ".PKGINFO")
        package = report["package"]
        for key, field in (("pkgname", "name"), ("pkgbase", "base"),
                           ("pkgver", "version"), ("arch", "architecture")):
            values = metadata.get(("", key), [])
            if len(values) != 1:
                errors.append(f".PKGINFO must contain one {key}")
            else:
                package[field] = values[0]
        package["licenses"] = metadata.get(("", "license"), [])
        if package.get("name") != PACKAGE or package.get("base") != PACKAGE:
            errors.append(".PKGINFO package identity is not niri-waylandshader")
        if not re.fullmatch(r"(?:[0-9]+:)?[A-Za-z0-9][A-Za-z0-9._+]*-[1-9][0-9]*(?:\.[0-9]+)*",
                            package.get("version", "")):
            errors.append(".PKGINFO has no supported Arch package version-release")
        if package.get("architecture") not in {"x86_64", "aarch64"}:
            errors.append(".PKGINFO architecture is not x86_64 or aarch64")

    if DESKTOP in texts:
        desktop = assignments(texts[DESKTOP], DESKTOP)
        for key in ("Exec", "TryExec"):
            values = desktop.get(("Desktop Entry", key), [])
            if len(values) != 1 or shlex.split(values[0]) not in (
                    [f"{PACKAGE}-session"], [f"/usr/bin/{PACKAGE}-session"]):
                errors.append(f"session desktop {key} must select the managed fork launcher")
        if desktop.get(("Desktop Entry", "Type")) != ["Application"]:
            errors.append("session desktop Type must be Application")
        names = desktop.get(("Desktop Entry", "DesktopNames"), [])
        if len(names) != 1 or [name for name in names[0].split(";") if name] != ["niri"]:
            errors.append("session desktop must retain the niri desktop identity")

    units = {
        name: assignments(text, name) for name, text in texts.items()
        if name.startswith("usr/lib/systemd/user/")
    }
    for name, unit in units.items():
        conflicts = shlex.split(" ".join(unit.get(("Unit", "Conflicts"), [])))
        if "niri.service" in conflicts:
            errors.append(f"unit would stop stock niri.service: {name}")

    def require(unit, path, key, expected):
        values = shlex.split(" ".join(unit.get(("Unit", key), [])))
        missing = set(expected) - set(values)
        if missing:
            errors.append(f"{path}: {key} missing {', '.join(sorted(missing))}")

    if SERVICE in units:
        unit = units[SERVICE]
        if unit.get(("Service", "Type"), [])[-1:] != ["notify"]:
            errors.append("fork compositor service must use Type=notify")
        commands = unit.get(("Service", "ExecStart"), [])
        if len(commands) != 1 or shlex.split(commands[0]) not in (
                [PACKAGE, "--session"], [f"/usr/bin/{PACKAGE}", "--session"]):
            errors.append("fork service must start the fork compositor with --session")
        require(unit, SERVICE, "BindsTo", ["graphical-session.target"])
        require(unit, SERVICE, "Before", ["graphical-session.target", "xdg-desktop-autostart.target"])
        require(unit, SERVICE, "Wants", ["graphical-session-pre.target", "xdg-desktop-autostart.target"])
        require(unit, SERVICE, "After", ["graphical-session-pre.target"])
    if SHUTDOWN in units:
        unit = units[SHUTDOWN]
        for key in ("Conflicts", "After"):
            require(unit, SHUTDOWN, key, ["graphical-session.target", "graphical-session-pre.target"])
        if unit.get(("Unit", "DefaultDependencies"), [])[-1:] not in (["no"], ["false"], ["0"]):
            errors.append("shutdown target must disable default dependencies")
        if unit.get(("Unit", "StopWhenUnneeded"), [])[-1:] not in (["yes"], ["true"], ["1"]):
            errors.append("shutdown target must stop when unneeded")


def inspect_archive(source, report):
    members = {}
    texts = {}
    reader = BoundedReader(source)
    # Use uncompressed, sequential reads: tarfile's compressed stream adapter may
    # allocate an entire expanded chunk before the caller can enforce a limit.
    with tarfile.open(fileobj=reader, mode="r:", encoding="utf-8", errors="strict") as archive:
        for member in archive:
            if len(members) >= MAX_MEMBERS:
                raise ValueError("archive exceeds the member-count limit")
            name = canonical_name(member.name, member.isdir())
            if "path" in member.pax_headers:
                canonical_name(member.pax_headers["path"], member.isdir())
            if name in members:
                raise ValueError(f"duplicate archive member: {name}")
            if name not in {".PKGINFO", ".BUILDINFO", ".MTREE"} and name.split("/")[0] != "usr":
                raise ValueError(f"unsupported installation root: {name}")
            if any(name == stock or name.startswith(stock + "/") or name.startswith(stock + ".d/")
                   for stock in STOCK_PATHS):
                raise ValueError(f"stock niri installation collision: {name}")
            if member.type not in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE) or member.issparse():
                raise ValueError(f"unsupported link, sparse or special member: {name}")
            if member.uid != 0 or member.gid != 0 or member.uname not in ("", "root") or member.gname not in ("", "root"):
                raise ValueError(f"member is not root-owned: {name}")
            if member.mode & 0o7022 or member.mode & 0o444 != 0o444:
                raise ValueError(f"unsafe special, writable or unreadable mode: {name}")
            if member.isdir() and (member.size != 0 or member.mode & 0o111 != 0o111):
                raise ValueError(f"invalid directory size or search permissions: {name}")
            if member.size < 0 or member.size > MAX_EXPANDED:
                raise ValueError(f"invalid or excessive member size: {name}")
            if any(key.startswith(("SCHILY.", "GNU.sparse")) for key in member.pax_headers):
                raise ValueError(f"unsupported extended permissions or sparse metadata: {name}")
            if member.isfile() and name.startswith("usr/lib/systemd/user/") and name not in {SERVICE, SHUTDOWN}:
                raise ValueError(f"unsupported additional systemd unit or drop-in: {name}")
            members[name] = member
            if member.isfile() and (name in {".PKGINFO", DESKTOP}
                                    or name.startswith("usr/lib/systemd/user/")):
                if member.size > MAX_TEXT:
                    raise ValueError(f"configuration exceeds the text-byte limit: {name}")
                with archive.extractfile(member) as payload:
                    texts[name] = payload.read(MAX_TEXT).decode("utf-8")
        # Reject a second hidden tar or nonzero trailer; draining also checks the
        # compressed stream's checksum/end marker instead of stopping at tar EOF.
        while data := reader.read(MAX_TEXT):
            if any(data):
                raise ValueError("nonzero data after the tar end marker")
    for name in members:
        parent = name.rpartition("/")[0]
        while parent:
            if parent in members and not members[parent].isdir():
                raise ValueError(f"non-directory ancestor of archive member: {name}")
            parent = parent.rpartition("/")[0]
    report["members_checked"] = len(members)
    report["expanded_bytes"] = reader.tell()
    check_contract(members, texts, report)


def verify_package(path):
    path = Path(path)
    report = {"schema_version": 1, "ok": False, "errors": [], "archive": str(path),
              "sha256": None, "package": {}, "limits": LIMITS}
    collection_errors = (OSError, ValueError, tarfile.TarError, EOFError, RecursionError)
    try:
        if not path.is_file():
            raise ValueError("archive must be an existing regular file")
        with ExitStack() as stack:
            source = stack.enter_context(path.open("rb"))
            before = os.fstat(source.fileno())
            if not stat.S_ISREG(before.st_mode) or before.st_size > MAX_ARCHIVE:
                raise ValueError("archive is not a regular file within the archive-byte limit")
            digest = hashlib.sha256()
            total = 0
            while chunk := source.read(1024 * 1024):
                total += len(chunk)
                if total > MAX_ARCHIVE:
                    raise ValueError("archive exceeds the archive-byte limit")
                digest.update(chunk)
            report["sha256"] = digest.hexdigest()
            report["archive_bytes"] = total
            source.seek(0)
            magic = source.read(6)
            source.seek(0)
            if magic.startswith(b"\x28\xb5\x2f\xfd") or path.suffix == ".zst":
                report["compression"] = "zstd"
                try:
                    from compression.zstd import DecompressionParameter, ZstdError, ZstdFile
                except ImportError:
                    raise ValueError("zstd inspection requires Python 3.14+ with compression.zstd; no dependencies are installed") from None
                collection_errors += (ZstdError,)
                expanded = stack.enter_context(ZstdFile(
                    source, options={DecompressionParameter.window_log_max: 26}))
            elif magic.startswith((b"\x1f\x8b", b"BZh", b"\xfd7zXZ", b"PK")):
                raise ValueError("unsupported compression/archive: only uncompressed tar and zstd are supported")
            else:
                report["compression"] = "none"
                expanded = source
            inspect_archive(expanded, report)
            after = os.fstat(source.fileno())
            if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                    after.st_size, after.st_mtime_ns, after.st_ctime_ns):
                raise ValueError("archive changed while being inspected; hash and inspection are not a stable snapshot")
    except collection_errors as error:
        report["errors"].append(str(error))
    report["ok"] = not report["errors"]
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "Exit 0: structural checks pass; 1: invalid/unsupported artifact; 2: usage. "
        "No extraction, execution, signature, ABI, runtime or license-compliance certification."))
    parser.add_argument("archive", type=Path, metavar="ARCHIVE")
    args = parser.parse_args()
    report = verify_package(args.archive)
    print(json.dumps(report, separators=(",", ":"), ensure_ascii=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
