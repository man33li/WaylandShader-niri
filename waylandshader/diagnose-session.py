#!/usr/bin/env python3
"""Collect privacy-limited Linux session evidence without changing the session."""

import argparse
from datetime import datetime, timezone
import errno
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys


ENV_KEYS = frozenset({
    "XDG_DATA_DIRS", "XDG_DATA_HOME", "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP",
    "XDG_SESSION_TYPE", "XDG_SESSION_ID", "XDG_RUNTIME_DIR", "DESKTOP_SESSION",
    "WAYLAND_DISPLAY", "DISPLAY", "NIRI_SOCKET", "DBUS_SESSION_BUS_ADDRESS",
    "HOME", "SHELL", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
    "NOCTALIA_CONFIG_HOME", "NOCTALIA_STATE_HOME",
})
PROCESS_NAMES = frozenset({"niri", "niri-waylandshader", "noctalia", "noctalia-shell"})
UNITS = (
    "niri.service", "niri-waylandshader.service", "graphical-session.target",
    "graphical-session-pre.target", "xdg-desktop-autostart.target",
    "xdg-desktop-portal.service", "xdg-desktop-portal-gnome.service",
    "xdg-desktop-portal-gtk.service", "pipewire.service", "pipewire-pulse.service",
    "wireplumber.service", "gnome-keyring-daemon.service", "gnome-keyring-daemon.socket",
)
UNIT_FIELDS = ("Id", "LoadState", "ActiveState", "SubState", "Result", "MainPID",
               "Requisite", "BindsTo", "PartOf", "Wants", "Before", "After", "FragmentPath")
NOTES = [
    "Exit 0 means evidence collected, not a healthy session; unavailable evidence remains unknown.",
    "/proc/PID/environ usually exposes the initial environment, not later in-process changes; manager environment is separate.",
    "PID UID/starttime/executable are checked before and after; fields are not an atomic snapshot and exec with the same executable may escape detection.",
    "Only allowlisted environment fields are emitted; paths and session identifiers may still be identifying. No argv is read.",
    "Unit state does not prove effective portal routing; no apps, portals, GIO discovery or services are activated.",
    "Journal excerpts omit raw messages and arbitrary metadata for privacy, retaining only recognized diagnostic signals.",
    "No visible journal records does not prove absence of events or unrestricted journal access.",
    "Process, manager and filesystem snapshots are current; --boot selects journal history only, never historical environments.",
    "SIGTERM may be normal shutdown. DMUB diagnostics alone do not establish a fork-specific cause or hardware fault.",
]


def unavailable(exc):
    reason = {errno.ENOENT: "disappeared_or_missing", errno.ESRCH: "disappeared_or_missing",
              errno.EACCES: "permission_denied", errno.EPERM: "permission_denied"}.get(exc.errno, "os_error")
    return {"status": "unavailable", "reason": reason, "errno": exc.errno}


def environment(raw):
    """Never treat a newline inside an environment value as a new assignment."""
    values = {}
    for entry in raw.split(b"\0"):
        key, sep, value = entry.partition(b"=")
        name = key.decode("ascii", errors="replace")
        if sep and name in ENV_KEYS:
            values[name] = value.decode("utf-8", errors="replace")
    return values


def identity(directory):
    uid = directory.stat().st_uid
    # comm may contain spaces or parentheses; fields after its final ')' are fixed.
    fields = (directory / "stat").read_bytes().rsplit(b")", 1)[1].split()
    return uid, int(fields[19]), os.readlink(directory / "exe")


def process(pid, uid, proc=Path("/proc")):
    directory = proc / str(pid)
    try:
        before = identity(directory)
    except OSError as exc:
        return {"pid": pid, **unavailable(exc)}
    except (ValueError, IndexError):
        return {"pid": pid, "status": "unavailable", "reason": "invalid_proc_identity"}
    if before[0] != uid:
        return {"pid": pid, "status": "unavailable", "reason": "not_owned_by_current_uid"}
    snapshot = {"pid": pid, "uid": uid, "starttime_ticks": before[1], "executable": before[2]}
    for key, reader in (
        ("environment", lambda: environment((directory / "environ").read_bytes())),
        ("cgroup", lambda: (directory / "cgroup").read_text(errors="replace").splitlines()),
        ("stdout", lambda: os.readlink(directory / "fd/1")),
        ("stderr", lambda: os.readlink(directory / "fd/2")),
    ):
        try:
            snapshot[key] = {"status": "ok", "value": reader()}
        except OSError as exc:
            snapshot[key] = unavailable(exc)
    try:
        after = identity(directory)
    except OSError as exc:
        return {"pid": pid, "status": "raced", "reason": "identity_unavailable_after_read", "detail": unavailable(exc)}
    except (ValueError, IndexError):
        return {"pid": pid, "status": "raced", "reason": "identity_invalid_after_read"}
    if before != after:
        return {"pid": pid, "status": "raced", "reason": "identity_changed_during_read"}
    snapshot["status"] = "ok" if all(snapshot[key]["status"] == "ok" for key in ("environment", "cgroup", "stdout", "stderr")) else "partial"
    return snapshot


def discover(uid, proc=Path("/proc")):
    pids, failures = [], {}
    try:
        for directory in proc.iterdir():
            if not directory.name.isdecimal():
                continue
            try:
                if directory.stat().st_uid == uid:
                    executable = os.readlink(directory / "exe")
                    if Path(executable.removesuffix(" (deleted)")).name in PROCESS_NAMES:
                        pids.append(int(directory.name))
            except OSError as exc:
                reason = unavailable(exc)["reason"]
                failures[reason] = failures.get(reason, 0) + 1
    except OSError as exc:
        return [], unavailable(exc)
    return sorted(pids), {"status": "partial" if failures else "ok", "uninspectable_entries": failures,
                          "matched": len(pids), "executable_basenames": sorted(PROCESS_NAMES)}


def command(arguments):
    try:
        result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, errors="replace", timeout=8, check=False,
                                env={**os.environ, "LC_ALL": "C", "SYSTEMD_PAGER": "cat"})
    except OSError as exc:
        return unavailable(exc), ""
    except subprocess.TimeoutExpired:
        return {"status": "unavailable", "reason": "timeout"}, ""
    status = {"status": "ok" if result.returncode == 0 else "unavailable", "returncode": result.returncode}
    if result.returncode:
        status["reason"] = "command_failed"
    # Never return raw stderr: it can contain arbitrary input or private metadata.
    if result.stderr.strip():
        status["diagnostics_present"] = True
        if re.search(r"permission|access denied|not seeing messages", result.stderr, re.I):
            status["access_limited"] = True
            if result.returncode == 0:
                status["status"] = "partial"
    return status, result.stdout


def systemd():
    manager, text = command(["systemctl", "--user", "show-environment", "--output=json"])
    if manager["status"] == "ok":
        try:
            values = json.loads(text)
            if not isinstance(values, dict) or not all(isinstance(value, str) for value in values.values()):
                raise ValueError
            manager["environment"] = {name: value for name, value in values.items() if name in ENV_KEYS}
        except ValueError:
            manager = {"status": "unavailable", "reason": "unparseable_manager_environment"}
    status, text = command(["systemctl", "--user", "show", "--no-pager",
                            "--property=" + ",".join(UNIT_FIELDS), *UNITS])
    units = {}
    for block in text.strip().split("\n\n"):
        values = dict(line.split("=", 1) for line in block.splitlines()
                      if "=" in line and line.split("=", 1)[0] in UNIT_FIELDS)
        if values.get("Id") in UNITS:
            units[values["Id"]] = values
    for unit in UNITS:
        if unit not in units:
            units[unit] = {"status": "unavailable", "reason": "no_unit_properties_returned"}
    return {"manager": manager, "query": status, "units": units}


def directory_state(path):
    try:
        return {"path": str(path), "status": "present" if stat.S_ISDIR(path.stat().st_mode) else "missing_or_not_directory"}
    except OSError as exc:
        if exc.errno == errno.ENOENT:
            return {"path": str(path), "status": "missing_or_not_directory"}
        return {"path": str(path), **unavailable(exc)}


def flatpak(processes):
    roots = {Path.home() / ".local/share/flatpak/exports/share", Path("/var/lib/flatpak/exports/share")}
    for values in [os.environ, *(item.get("environment", {}).get("value", {}) for item in processes)]:
        data_home = values.get("XDG_DATA_HOME")
        if data_home and Path(data_home).is_absolute():
            roots.add(Path(data_home) / "flatpak/exports/share")
    query, text = command(["flatpak", "--installations"])
    if query["status"] == "ok":
        for line in text.splitlines():
            if line.strip() and Path(line.strip()).is_absolute():
                roots.add(Path(line.strip()) / "exports/share")
    return {"installations_query": query, "export_roots": [directory_state(root) for root in sorted(roots)]}


def findings(processes, services, exports):
    result = []
    units = services["units"]
    graphical = units["graphical-session.target"]
    if graphical.get("LoadState") == "loaded" and graphical.get("ActiveState") == "inactive":
        result.append({"code": "graphical_target_inactive", "advisory": "If this is the graphical login session, its target is inactive; GNOME portal Requisite may prevent startup."})
    manager = services["manager"].get("environment", {})
    for item in processes:
        pid = item["pid"]
        if item["status"] not in {"ok", "partial"}:
            result.append({"code": "process_evidence_unavailable", "pid": pid, "advisory": "No verified process snapshot; do not infer session health."})
            continue
        values = item.get("environment", {}).get("value")
        if values is None:
            continue
        dirs = values.get("XDG_DATA_DIRS", "").split(":")
        missing = [root["path"] for root in exports["export_roots"] if root["status"] == "present" and root["path"] not in dirs]
        if missing:
            result.append({"code": "export_roots_not_in_initial_environment", "pid": pid, "paths": missing,
                           "advisory": "If this process discovers Flatpak desktop apps via XDG_DATA_DIRS, these export roots are absent from its initial environment."})
        mismatches = [key for key in ("XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
                                     "XDG_SESSION_ID", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS")
                      if key in values and key in manager and values[key] != manager[key]]
        if mismatches:
            result.append({"code": "session_identity_mismatch", "pid": pid, "keys": mismatches,
                           "advisory": "Manager and initial process identity differ; stale manager state or an intentional nested/multi-session setup are possible."})
    absent = [root["path"] for root in exports["export_roots"] if root["status"] == "missing_or_not_directory"]
    if absent:
        result.append({"code": "export_roots_missing", "paths": absent,
                       "advisory": "Expected roots are absent or not directories; this is normal when that Flatpak installation has no exports."})
    return result


JOURNAL_SIGNALS = {
    "dmub": r"\b(?:DMUB|DMCUB)\b", "queue_full": r"queue.{0,20}full",
    "dmub_queue_status_2": r"Error queueing DMUB command:\s*status=2\b",
    "sigterm": r"\bSIGTERM\b|signal 15\b", "requisite": r"\brequisite\b",
    "dependency_failure": r"dependency failed|failed with result ['\"]dependency",
    "failure": r"\bfailed\b|\bfailure\b|\berror\b", "xdg_data_dirs": r"\bXDG_DATA_DIRS\b",
}


def journal(boot, lines):
    excerpts = {}
    for scope, selection in (("user", ["--user", *(arg for unit in UNITS for arg in ("--unit", unit))]),
                             ("kernel", ["--kernel", "--grep=amdgpu|drm|DMUB|dmub"])):
        status, text = command(["journalctl", *selection, "--boot", str(boot), "--lines", str(lines),
                                "--output=json", "--no-pager"])
        records = []
        malformed = 0
        if status["status"] in {"ok", "partial"}:
            for line in text.splitlines():
                try:
                    entry = json.loads(line)
                    if not isinstance(entry, dict):
                        raise ValueError
                except ValueError:
                    malformed += 1
                    continue
                message = entry.get("MESSAGE", "")
                record = {"signals": [name for name, pattern in JOURNAL_SIGNALS.items()
                                      if isinstance(message, str) and re.search(pattern, message, re.I)]}
                timestamp = entry.get("__REALTIME_TIMESTAMP", "")
                if isinstance(timestamp, str) and timestamp.isdecimal():
                    record["timestamp_us"] = timestamp
                boot_id = entry.get("_BOOT_ID", "")
                if isinstance(boot_id, str) and re.fullmatch(r"[0-9a-f]{32}", boot_id):
                    record["boot_id"] = boot_id
                pid = entry.get("_PID", "")
                if isinstance(pid, str) and pid.isdecimal():
                    record["pid"] = int(pid)
                priority = entry.get("PRIORITY")
                if isinstance(priority, str) and priority in ("0", "1", "2", "3", "4", "5", "6", "7"):
                    record["priority"] = priority
                unit = entry.get("_SYSTEMD_USER_UNIT", entry.get("UNIT"))
                if unit in UNITS:
                    record["unit"] = unit
                records.append(record)
            status["visibility"] = "visible_records" if records else "no_visible_records"
            if malformed:
                status["status"] = "partial"
                status["malformed_records"] = malformed
        excerpts[scope] = {**status, "records": records}
    return {"boot": boot, "lines_per_scope": lines, "excerpts": excerpts}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # Do not echo unrecognized arguments, which could contain credentials.
        raise ValueError("invalid_arguments; use --help")


def positive_pid(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("PID must be positive")
    return number


def line_count(value):
    number = int(value)
    if not 1 <= number <= 500:
        raise argparse.ArgumentTypeError("lines must be between 1 and 500")
    return number


def capture():
    snapshot = {"started_at_utc": datetime.now(timezone.utc).isoformat(),
                "snapshot_scope": "current", "kernel_release": os.uname().release}
    try:
        snapshot["boot_id"] = {"status": "ok", "value": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
    except OSError as exc:
        snapshot["boot_id"] = unavailable(exc)
    return snapshot


def main(argv=None):
    parser = Parser(description=__doc__, epilog=(
        "Read-only: no service activation, config writes, sudo or network. Privacy: no argv or full environments; "
        "allowlisted session fields and paths remain identifying. Journal is opt-in and raw messages are omitted. "
        "JSON report on stdout. Exit 0: collected (NOT a health verdict); 1: fatal collection failure; 2: invalid usage."))
    parser.add_argument("--pid", type=positive_pid, action="append", help="owned PID; repeatable; otherwise discover exact session executable basenames")
    parser.add_argument("--journal", action="store_true", help="include privacy-filtered user/kernel journal JSON excerpts")
    parser.add_argument("--boot", type=int, default=0, help="journal boot offset, including negatives; other snapshots stay current (default: 0)")
    parser.add_argument("--lines", type=line_count, default=80, help="maximum journal records per scope, 1..500 (default: 80)")
    try:
        args = parser.parse_args(argv)
    except ValueError as exc:
        print(json.dumps({"schema_version": 1, "status": "usage_error", "error": str(exc)}))
        return 2
    try:
        if not sys.platform.startswith("linux"):
            raise RuntimeError("Linux /proc is required")
        captured = capture()
        uid = os.getuid()
        pids, discovery = (list(dict.fromkeys(args.pid)), {"status": "explicit_selection"}) if args.pid else discover(uid)
        processes = [process(pid, uid) for pid in pids]
        if not args.pid:
            processes = [item if "executable" not in item or Path(item["executable"].removesuffix(" (deleted)")).name in PROCESS_NAMES
                         else {"pid": item["pid"], "status": "raced", "reason": "executable_changed_since_discovery"} for item in processes]
        services = systemd()
        exports = flatpak(processes)
        report = {"schema_version": 1, "status": "collected", "uid": uid, "capture": captured, "discovery": discovery,
                  "processes": processes, "systemd": services, "flatpak": exports,
                  "findings": findings(processes, services, exports), "notes": NOTES,
                  "journal": journal(args.boot, args.lines) if args.journal else {"status": "not_requested"}}
        if not processes:
            report["findings"].append({"code": "no_selected_processes", "advisory": "No matching owned executable was observed; this does not establish session health. Script-hosted shells may require --pid."})
        print(json.dumps(report, ensure_ascii=True, separators=(",", ":")))
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        # Exception strings can carry paths/input; retain only the exception class.
        print(json.dumps({"schema_version": 1, "status": "fatal", "error": type(exc).__name__}))
        return 1


if __name__ == "__main__":
    sys.exit(main())
