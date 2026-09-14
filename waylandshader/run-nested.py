#!/usr/bin/env python3
"""Preview the built compositor on a private D-Bus with temporary shader settings."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    executable = root / "build/niri-install/bin/niri-waylandshader"
    if not executable.is_file():
        sys.exit("Build first: python3 waylandshader/build.py")
    if not os.environ.get("WAYLAND_DISPLAY"):
        sys.exit("Run this launcher from a terminal in your existing Wayland desktop.")
    if not shutil.which("dbus-run-session"):
        sys.exit("dbus-run-session is required for the isolated preview.")
    with tempfile.TemporaryDirectory(prefix="waylandshader-nested-") as temporary:
        env = os.environ.copy()
        env.pop("NIRI_SOCKET", None)
        env.pop("WAYLAND_SOCKET", None)
        env.pop("DISPLAY", None)
        env["PATH"] = str(executable.parent) + os.pathsep + env.get("PATH", "")
        env["XDG_CONFIG_HOME"] = temporary + "/config"
        env["XDG_CACHE_HOME"] = temporary + "/cache"
        env["XDG_DATA_HOME"] = temporary + "/data"
        env["XDG_STATE_HOME"] = temporary + "/state"
        env["WAYLANDSHADER_CONFIG"] = temporary + "/settings.json"
        env["WAYLANDSHADER_DBUS_SERVICE"] = "org.waylandshader.Niri"
        env["NIRI_DISABLE_SYSTEM_MANAGER_NOTIFY"] = "1"
        arguments = ["dbus-run-session", "--", str(executable), "--config", str(root / "waylandshader/nested.kdl")]
        command = sys.argv[1:]
        if command and command[0] == "--":
            command = command[1:]
        if command:
            arguments += ["--", *command]
        print("Nested preview: temporary settings and a private D-Bus; stock niri is untouched.", flush=True)
        print("Effects apply ONLY inside this nested window, not to physical monitors.", flush=True)
        print("For monitor-wide effects, install the package and select 'niri (WaylandShader)' at login.", flush=True)
        print("Close its window to exit. Nested shortcuts: Alt+Shift+E to quit, Alt+Return for alacritty if installed.", flush=True)
        try:
            return subprocess.call(arguments, env=env)
        except KeyboardInterrupt:
            return 130


if __name__ == "__main__":
    sys.exit(main())
