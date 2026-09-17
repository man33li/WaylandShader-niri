"""Structural package boundaries; every archive payload is inert fixture data."""

import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "verify-package.py"
SPEC = importlib.util.spec_from_file_location("package_inspection", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)
SERVICE = "usr/lib/systemd/user/niri-waylandshader.service"
SHUTDOWN = "usr/lib/systemd/user/niri-waylandshader-shutdown.target"
DESKTOP = "usr/share/wayland-sessions/niri-waylandshader.desktop"
SERVICE_TEXT = """[Unit]
BindsTo=graphical-session.target
Before=graphical-session.target
Wants=graphical-session-pre.target
After=graphical-session-pre.target
Wants=xdg-desktop-autostart.target
Before=xdg-desktop-autostart.target
[Service]
Type=notify
ExecStart="/usr/bin/niri-waylandshader" --session
"""


class PackageInspectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="waylandshader-package-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.files = {
            ".PKGINFO": "pkgname = niri-waylandshader\npkgbase = niri-waylandshader\npkgver = 26.04.ws0.2.2-2\narch = x86_64\nlicense = MIT\n",
            "usr/bin/niri-waylandshader": "inert compositor fixture\n",
            "usr/bin/niri-waylandshader-session": "inert launcher fixture\n",
            "usr/bin/waylandshader-niri-controller": "inert controller fixture\n",
            "usr/bin/waylandshader-nirictl": "inert CLI fixture\n",
            "usr/lib/waylandshader/libwaylandshader-rashader.so.2": "inert library fixture\n",
            "usr/lib/dinit.d/user/niri-waylandshader": "inert dinit fixture\n",
            "usr/lib/dinit.d/user/niri-waylandshader.target": "inert dinit target fixture\n",
            "usr/share/applications/org.waylandshader.NiriController.desktop": "inert controller entry\n",
            DESKTOP: "[Desktop Entry]\nType=Application\nExec=niri-waylandshader-session\nTryExec=niri-waylandshader-session\nDesktopNames=niri;\n",
            SERVICE: SERVICE_TEXT,
            SHUTDOWN: "[Unit]\nDefaultDependencies=no\nStopWhenUnneeded=true\nConflicts=graphical-session.target graphical-session-pre.target\nAfter=graphical-session.target graphical-session-pre.target\n",
        }
        for name in ("README.md", "REFACTORING.md", "HISTORY.md", "UPGRADING.md"):
            self.files[f"usr/share/doc/niri-waylandshader/{name}"] = "inert documentation\n"
        for name in ("niri-GPL-3.0", "waylandshader-MIT", "librashader-MPL-2.0"):
            self.files[f"usr/share/licenses/niri-waylandshader/{name}"] = "inert notice\n"

    def archive(self, extra=None, overrides=None):
        path = self.root / "fixture.pkg.tar"
        with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as archive:
            for name, text in self.files.items():
                content = text.encode()
                member = tarfile.TarInfo(name)
                member.size = len(content)
                member.mode = 0o755 if name.startswith("usr/bin/") else 0o644
                for key, value in (overrides or {}).get(name, {}).items():
                    setattr(member, key, value)
                archive.addfile(member, io.BytesIO(content))
            if extra is not None:
                archive.addfile(extra)
        return path

    def assert_rejected(self, path):
        report = CHECKER.verify_package(path)
        self.assertFalse(report["ok"], report)
        self.assertTrue(report["errors"], report)
        return report

    def test_cli_accepts_managed_session_with_repeated_relationships(self):
        path = self.archive()
        result = subprocess.run([sys.executable, str(SCRIPT), str(path)],
                                text=True, capture_output=True, timeout=10)
        report = json.loads(result.stdout)
        self.assertEqual(result.returncode, 0, report)
        self.assertTrue(report["ok"], report)
        self.assertEqual(report["package"]["version"], "26.04.ws0.2.2-2")
        self.assertEqual(report["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_raw_session_without_manager_assets_is_rejected(self):
        self.files[".PKGINFO"] = self.files[".PKGINFO"].replace("0.2.2-2", "0.2.2-1")
        self.files[DESKTOP] = self.files[DESKTOP].replace("Exec=niri-waylandshader-session", "Exec=niri-waylandshader --session")
        for name in (SERVICE, SHUTDOWN, "usr/bin/niri-waylandshader-session",
                     "usr/lib/dinit.d/user/niri-waylandshader", "usr/lib/dinit.d/user/niri-waylandshader.target"):
            del self.files[name]
        self.assert_rejected(self.archive())

    def test_relationship_reset_and_stock_conflict_are_rejected(self):
        for broken in (
            SERVICE_TEXT.replace("Wants=xdg-desktop-autostart.target", "Wants=\nWants=xdg-desktop-autostart.target"),
            SERVICE_TEXT.replace("Before=graphical-session.target\n", ""),
            SERVICE_TEXT.replace("[Service]", "Conflicts=niri.service\n[Service]"),
            SERVICE_TEXT.replace('ExecStart="/usr/bin/niri-waylandshader"', 'ExecStart="/usr/bin/niri"'),
        ):
            with self.subTest(service=broken):
                self.files[SERVICE] = broken
                self.assert_rejected(self.archive())

    def test_unsafe_names_duplicates_and_stock_paths_are_rejected(self):
        for name in (
            "../escape", "/escape", "usr/bin/../escape", "./usr/bin/escape", "usr//bin/escape",
            "usr/bin/niri-waylandshader", "usr/bin/niri", "usr/bin/niri-session",
            "usr/share/wayland-sessions/niri.desktop", "usr/lib/systemd/user/niri.service",
            "usr/lib/systemd/user/niri-shutdown.target", "usr/lib/systemd/user/niri.service.d/override.conf",
        ):
            with self.subTest(name=name):
                self.assert_rejected(self.archive(extra=tarfile.TarInfo(name)))
        self.assertFalse((self.root / "escape").exists())

    def test_unsafe_ownership_modes_links_and_file_ancestors_are_rejected(self):
        for attributes in ({"uid": 1000}, {"gid": 1000}, {"mode": 0o4755},
                           {"mode": 0o775}, {"mode": 0o757}, {"mode": 0o644}):
            with self.subTest(attributes=attributes):
                self.assert_rejected(self.archive(overrides={"usr/bin/niri-waylandshader": attributes}))
        link = tarfile.TarInfo("usr/bin/alias")
        link.type = tarfile.SYMTYPE
        link.linkname = "../escape"
        self.assert_rejected(self.archive(extra=link))
        self.assert_rejected(self.archive(extra=tarfile.TarInfo("usr/bin")))
        # Directory normalization must not hide a collision with a required file.
        directory = tarfile.TarInfo("usr/bin/niri-waylandshader//")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        self.assert_rejected(self.archive(extra=directory))
        self.assert_rejected(self.archive(extra=tarfile.TarInfo(
            "usr/lib/systemd/user/niri-waylandshader.service.d/override.conf")))

    def test_metadata_identity_and_bounded_config_are_rejected(self):
        original = self.files[".PKGINFO"]
        for metadata in (original.replace("pkgname = niri-waylandshader", "pkgname = niri"),
                         original + "pkgver = 26.04.ws0.2.2-1\n",
                         "#" * (64 * 1024 + 1)):
            with self.subTest(size=len(metadata)):
                self.files[".PKGINFO"] = metadata
                self.assert_rejected(self.archive())

    def test_truncated_payload_and_hidden_second_archive_are_rejected(self):
        path = self.archive()
        contents = path.read_bytes()
        path.write_bytes(contents[:700])
        self.assert_rejected(path)
        path.write_bytes(contents + contents)
        self.assert_rejected(path)

    def test_zstd_stream_and_truncated_frame(self):
        try:
            from compression.zstd import compress
        except ImportError:
            self.skipTest("stdlib zstd requires Python 3.14+")
        path = self.archive()
        compressed = compress(path.read_bytes())
        zstd_path = path.with_suffix(".tar.zst")
        zstd_path.write_bytes(compressed)
        self.assertTrue(CHECKER.verify_package(zstd_path)["ok"])
        zstd_path.write_bytes(compressed[:-1])
        self.assert_rejected(zstd_path)


if __name__ == "__main__":
    unittest.main()
