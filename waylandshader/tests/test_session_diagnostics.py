"""Privacy and unknown-state regressions without touching live services."""

import errno
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "session_diagnostics", Path(__file__).resolve().parents[1] / "diagnose-session.py"
)
diagnostics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostics)


class SessionDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="session-evidence-test-")
        self.addCleanup(temporary.cleanup)
        self.proc = Path(temporary.name)
        self.directory = self.proc / "17"
        self.directory.mkdir()
        (self.directory / "stat").write_text("17 (name with ) parentheses) S " + "0 " * 18 + "42\n")
        (self.directory / "exe").symlink_to("/usr/bin/niri")
        (self.directory / "cgroup").write_text("0::/user.slice/session.scope\n")
        (self.directory / "environ").write_bytes(
            b"TOKEN=private-token\nXDG_CURRENT_DESKTOP=forged\0"
            b"XDG_DATA_DIRS=/usr/share:/exports/share\0WAYLAND_DISPLAY=wayland-1\0"
        )
        (self.directory / "fd").mkdir()
        for fd in ("1", "2"):
            (self.directory / "fd" / fd).symlink_to("/dev/null")

    def test_nul_boundaries_do_not_promote_secret_lines_to_environment(self):
        report = diagnostics.process(17, os.getuid(), self.proc)
        self.assertEqual(report["status"], "ok")
        self.assertEqual(report["environment"]["value"], {
            "XDG_DATA_DIRS": "/usr/share:/exports/share", "WAYLAND_DISPLAY": "wayland-1",
        })
        self.assertNotIn("private-token", json.dumps(report))
        self.assertNotIn("forged", json.dumps(report))

    def test_manager_json_preserves_paths_without_exposing_secrets(self):
        values = {"XDG_DATA_DIRS": "/data/with space:/data/é",
                  "XDG_SESSION_ID": "42", "TOKEN": "private-token\nXDG_DATA_DIRS=forged"}
        with mock.patch.object(diagnostics, "command", side_effect=[
            ({"status": "ok"}, json.dumps(values)), ({"status": "ok"}, ""),
        ]):
            report = diagnostics.systemd()
        self.assertEqual(report["manager"]["environment"]["XDG_DATA_DIRS"], "/data/with space:/data/é")
        self.assertNotIn("private-token", json.dumps(report))
        self.assertNotIn("forged", json.dumps(report))

    def test_reused_pid_discards_all_attributed_fields(self):
        with mock.patch.object(diagnostics, "identity", side_effect=[
            (os.getuid(), 42, "/usr/bin/niri"), (os.getuid(), 43, "/usr/bin/niri"),
        ]):
            report = diagnostics.process(17, os.getuid(), self.proc)
        self.assertEqual(report["status"], "raced")
        self.assertNotIn("environment", report)
        self.assertNotIn("executable", report)

    def test_disappearance_and_permission_are_unknown_not_healthy(self):
        self.assertEqual(diagnostics.process(18, os.getuid(), self.proc)["status"], "unavailable")
        for failure in (PermissionError(errno.EACCES, "private detail"), FileNotFoundError(errno.ENOENT, "private detail")):
            with self.subTest(failure=type(failure).__name__), mock.patch.object(diagnostics, "identity", side_effect=failure):
                report = diagnostics.process(17, os.getuid(), self.proc)
            self.assertEqual(report["status"], "unavailable")
            self.assertNotIn("environment", report)
            self.assertNotIn("private detail", json.dumps(report))
        with mock.patch.object(diagnostics, "identity", side_effect=[
            (os.getuid(), 42, "/usr/bin/niri"), FileNotFoundError(errno.ENOENT, "gone"),
        ]):
            report = diagnostics.process(17, os.getuid(), self.proc)
        self.assertEqual(report["status"], "raced")
        self.assertNotIn("environment", report)

    def test_foreign_uid_is_not_inspected(self):
        report = diagnostics.process(17, os.getuid() + 1, self.proc)
        self.assertEqual(report["status"], "unavailable")
        self.assertNotIn("environment", report)
        self.assertNotIn("executable", report)

    def test_inaccessible_export_is_not_reported_missing(self):
        path = mock.Mock()
        path.stat.side_effect = PermissionError(errno.EACCES, "denied")
        report = diagnostics.directory_state(path)
        self.assertEqual(report["status"], "unavailable")
        self.assertEqual(report["reason"], "permission_denied")

    def test_journal_never_emits_raw_messages_or_command_lines(self):
        record = json.dumps({
            "MESSAGE": "DMUB queue full; token=private-token; argv=private-command",
            "_CMDLINE": "private-command", "__REALTIME_TIMESTAMP": "1234", "PRIORITY": "3",
        })
        with mock.patch.object(diagnostics, "command", side_effect=[
            ({"status": "ok"}, ""), ({"status": "ok"}, record),
        ]):
            report = diagnostics.journal(-1, 10)
        self.assertNotIn("private-token", json.dumps(report))
        self.assertNotIn("private-command", json.dumps(report))
        self.assertEqual(set(report["excerpts"]["kernel"]["records"][0]["signals"]), {"dmub", "queue_full"})
        self.assertEqual(report["excerpts"]["user"]["visibility"], "no_visible_records")

    def test_missing_process_creates_unknown_evidence_advisory(self):
        process = diagnostics.process(18, os.getuid(), self.proc)
        findings = diagnostics.findings([process], {
            "manager": {"status": "unavailable"},
            "units": {"graphical-session.target": {"status": "unavailable"}},
        }, {"export_roots": []})
        self.assertEqual({item["code"] for item in findings}, {"process_evidence_unavailable"})


if __name__ == "__main__":
    unittest.main()
