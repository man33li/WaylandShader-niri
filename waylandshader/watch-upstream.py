#!/usr/bin/env python3
"""Install, inspect, or remove a weekly current-user niri candidate-build timer."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent.parent
SERVICE = "waylandshader-niri-upstream.service"
TIMER = "waylandshader-niri-upstream.timer"
OWNER = "# Managed by WaylandShader-niri watch-upstream.py; do not edit.\n"
DIGEST = "# Content-SHA256: "


def xdg_directory(variable, fallback):
    value = os.environ.get(variable)
    path = Path(value) if value else Path.home() / fallback
    if not path.is_absolute():
        raise RuntimeError(f"{variable} must be an absolute path")
    return path.resolve()


def quote(value):
    """Quote one systemd word, escaping specifiers (not shell quoting)."""
    value = str(value)
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise RuntimeError("Systemd paths and environment values must not contain control characters")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%") + '"'


def owned_unit(body):
    digest = hashlib.sha256(body.encode()).hexdigest()
    return OWNER + DIGEST + digest + "\n" + body


def unit_contents(report):
    command = " ".join(quote(arg) for arg in (
        Path(sys.executable).absolute(), ROOT / "waylandshader/check-upstream.py",
        "--build", "--report", report,
    ))
    # ':' disables environment-variable expansion, preserving literal '$' in paths.
    service = (
        "[Unit]\n"
        "Description=Merge-check official niri releases against the committed WaylandShader fork\n\n"
        "[Service]\n"
        "Type=oneshot\n"
        f"Environment={quote('PATH=' + os.environ.get('PATH', os.defpath))}\n"
        f"ExecStart=:{command}\n"
        "Nice=10\n"
        "TimeoutStartSec=infinity\n"
    )
    timer = (
        "[Unit]\n"
        "Description=Weekly WaylandShader niri fork merge and candidate build check\n\n"
        "[Timer]\n"
        "OnCalendar=weekly\n"
        "RandomizedDelaySec=30m\n"
        "Persistent=true\n"
        f"Unit={SERVICE}\n\n"
        "[Install]\n"
        "WantedBy=timers.target\n"
    )
    return {SERVICE: owned_unit(service), TIMER: owned_unit(timer)}


def read_owned(path):
    if path.is_symlink():
        raise RuntimeError(f"Refusing to modify a symlink: {path}")
    if not path.exists():
        return None
    if not path.is_file():
        raise RuntimeError(f"Not a regular unit file: {path}")
    text = path.read_text()
    if not text.startswith(OWNER + DIGEST):
        raise RuntimeError(f"Refusing unrelated unit: {path}")
    digest, separator, body = text[len(OWNER + DIGEST):].partition("\n")
    if not separator or digest != hashlib.sha256(body.encode()).hexdigest():
        raise RuntimeError(f"Refusing customized unit: {path}; restore or remove it manually")
    return text


def systemctl(*args):
    subprocess.run(["systemctl", "--user", *args], check=True)


def check_effective_unit(path):
    """Do not shadow another installation or activate units with custom drop-ins."""
    dropins = path.with_name(path.name + ".d")
    if dropins.is_symlink() or (dropins.exists() and any(dropins.iterdir())):
        raise RuntimeError(f"Refusing custom unit drop-ins: {dropins}")
    result = subprocess.run(
        ["systemctl", "--user", "show", path.name, "--property=LoadState",
         "--property=FragmentPath", "--property=DropInPaths"],
        capture_output=True, text=True,
    )
    properties = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    if result.returncode and properties.get("LoadState") != "not-found":
        raise RuntimeError(result.stderr.strip() or "Cannot query the current-user systemd manager")
    fragment = properties.get("FragmentPath")
    if fragment and Path(fragment).resolve() != path.resolve():
        raise RuntimeError(f"Refusing unit already supplied by {fragment}")
    if properties.get("DropInPaths"):
        raise RuntimeError(f"Refusing custom drop-ins for {path.name}: {properties['DropInPaths']}")


def install(unit_dir, report):
    checker = ROOT / "waylandshader/check-upstream.py"
    if not checker.is_file():
        raise RuntimeError(f"Checker not found: {checker}")
    units = unit_contents(report)
    existing = {}
    # Check both units before writing anything or changing the user's manager.
    for name, content in units.items():
        path = unit_dir / name
        existing[name] = read_owned(path)
        if existing[name] is not None and existing[name] != content:
            raise RuntimeError(
                f"{path} has different installation settings; uninstall its owned units first"
            )
        check_effective_unit(path)
    unit_dir.mkdir(parents=True, exist_ok=True)
    report.parent.mkdir(parents=True, exist_ok=True)
    for name, content in units.items():
        if existing[name] is None:
            # Exclusive creation also refuses a conflicting file created after preflight.
            with (unit_dir / name).open("x") as file:
                file.write(content)
    systemctl("daemon-reload")
    systemctl("enable", "--now", TIMER)
    print(f"Enabled {TIMER}: weekly, with up to 30 minutes of randomized delay.")
    print(f"Report: {report}")
    print("Runs while your user manager is available; no compositor service was changed.")


def uninstall(unit_dir):
    existing = {}
    for name in (SERVICE, TIMER):
        path = unit_dir / name
        existing[name] = read_owned(path)
        check_effective_unit(path)
    if existing[TIMER] is not None:
        systemctl("disable", "--now", TIMER)
    if existing[SERVICE] is not None:
        systemctl("stop", SERVICE)
    for name, content in existing.items():
        if content is not None:
            path = unit_dir / name
            if read_owned(path) != content:
                raise RuntimeError(f"Unit changed during uninstall; leaving it in place: {path}")
            path.unlink()
    if any(content is not None for content in existing.values()):
        systemctl("daemon-reload")
    print("Owned upstream-check units removed; reports and source/build trees retained.")


def status(unit_dir, report):
    print(f"Timer: {TIMER}", flush=True)
    print(f"Unit directory: {unit_dir}", flush=True)
    print(f"Report path for this checkout's installation: {report}", flush=True)
    print("Timer state is not a compatibility result; inspect the checker report and logs.", flush=True)
    return subprocess.run([
        "systemctl", "--user", "status", "--no-pager", TIMER,
    ]).returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("install", "status", "uninstall"))
    args = parser.parse_args()
    if os.getuid() == 0 or os.geteuid() == 0 or os.environ.get("SUDO_USER"):
        parser.error("Run as the desktop user, without sudo; root installation is not supported")
    unit_dir = xdg_directory("XDG_CONFIG_HOME", ".config") / "systemd/user"
    report = xdg_directory("XDG_STATE_HOME", ".local/state") / "waylandshader/niri-upstream.json"
    if args.command == "install":
        install(unit_dir, report)
    elif args.command == "uninstall":
        uninstall(unit_dir)
    else:
        return status(unit_dir, report)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
