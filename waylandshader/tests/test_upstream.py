"""Real-Git regressions for preserving a maintainer's source and merge work."""

import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class UpstreamCandidateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="waylandshader-git-test-")
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name).resolve() / "repo"
        self.repo.mkdir()
        self.git("init", "--object-format=sha1", "-b", "main")
        self.git("config", "user.name", "WaylandShader test")
        self.git("config", "user.email", "test@localhost")
        (self.repo / ".gitignore").write_text("/build/\n__pycache__/\n")
        (self.repo / "shared.txt").write_text("baseline\n")
        (self.repo / "waylandshader").mkdir()
        script = self.repo / "waylandshader/check-upstream.py"
        shutil.copy2(Path(__file__).resolve().parents[1] / "check-upstream.py", script)
        self.commit("baseline")

        spec = importlib.util.spec_from_file_location("upstream_checker", script)
        self.checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.checker)
        self.candidate = self.repo / "build/candidate"
        self.state = self.repo / "build/preparation.json"
        self.log = io.StringIO()

    def git(self, *args, cwd=None):
        result = subprocess.run(
            ["git", "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false", *args],
            cwd=cwd or self.repo,
            env={key: value for key, value in os.environ.items() if not key.startswith("GIT_")},
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        )
        return result.stdout.strip()

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-m", message)
        return self.git("rev-parse", "HEAD")

    def diverge(self, conflict=False):
        self.git("switch", "-c", "upstream-fixture")
        (self.repo / ("shared.txt" if conflict else "upstream.txt")).write_text("upstream change\n")
        upstream = self.commit("upstream change")
        self.git("switch", "main")
        (self.repo / ("shared.txt" if conflict else "shader.txt")).write_text("fork shader change\n")
        fork = self.commit("fork change")
        return fork, upstream

    def prepare(self, fork, upstream):
        self.checker.prepare_candidate(self.candidate, self.state, fork, upstream, self.log)

    def test_merge_combines_both_sides_without_changing_main(self):
        fork, upstream = self.diverge()
        self.prepare(fork, upstream)
        self.assertEqual((self.candidate / "upstream.txt").read_text(), "upstream change\n")
        self.assertEqual((self.candidate / "shader.txt").read_text(), "fork shader change\n")
        self.assertFalse((self.repo / "upstream.txt").exists())
        self.assertEqual(self.git("rev-parse", "HEAD"), fork)
        self.assertEqual(self.git("status", "--porcelain"), "")
        self.assertEqual(self.git("rev-parse", "MERGE_HEAD", cwd=self.candidate), upstream)
        self.prepare(fork, upstream)
        self.assertEqual((self.candidate / "shader.txt").read_text(), "fork shader change\n")

    def test_conflicting_candidate_and_manual_resolution_survive_retries(self):
        fork, upstream = self.diverge(conflict=True)
        with self.assertRaises(RuntimeError):
            self.prepare(fork, upstream)
        conflicted = (self.candidate / "shared.txt").read_bytes()
        unmerged = self.git("ls-files", "--unmerged", cwd=self.candidate)
        self.assertNotEqual(unmerged, "")
        with self.assertRaises(RuntimeError):
            self.prepare(fork, upstream)
        self.assertEqual((self.candidate / "shared.txt").read_bytes(), conflicted)
        self.assertEqual(self.git("ls-files", "--unmerged", cwd=self.candidate), unmerged)

        (self.candidate / "shared.txt").write_text("maintainer's conflict resolution\n")
        self.git("add", "shared.txt", cwd=self.candidate)
        with self.assertRaises(RuntimeError):
            self.prepare(fork, upstream)
        self.assertEqual((self.candidate / "shared.txt").read_text(), "maintainer's conflict resolution\n")
        self.assertEqual(self.git("diff", "--name-only", cwd=self.candidate), "")
        self.assertEqual(self.git("rev-parse", "HEAD"), fork)

        retained = self.repo.parent / "retained-candidate"
        self.git("worktree", "move", str(self.candidate), str(retained))
        self.state.rename(self.repo.parent / "retained-preparation.json")
        with self.assertRaises(RuntimeError):
            self.prepare(fork, upstream)
        self.assertEqual((retained / "shared.txt").read_text(), "maintainer's conflict resolution\n")
        self.assertEqual(self.git("ls-files", "--unmerged", cwd=self.candidate), unmerged)

    def test_staged_edits_to_prepared_candidate_are_not_discarded(self):
        fork, upstream = self.diverge()
        self.prepare(fork, upstream)
        (self.candidate / "shader.txt").write_text("maintainer's staged shader fix\n")
        self.git("add", "shader.txt", cwd=self.candidate)
        with self.assertRaises(RuntimeError):
            self.prepare(fork, upstream)
        self.assertEqual((self.candidate / "shader.txt").read_text(), "maintainer's staged shader fix\n")
        self.assertEqual(self.git("diff", "--name-only", cwd=self.candidate), "")
        self.assertEqual(self.git("rev-parse", "HEAD"), fork)

    def test_dirty_source_is_refused_but_ignored_build_output_is_allowed(self):
        revision = self.git("rev-parse", "HEAD")
        (self.repo / "build").mkdir()
        (self.repo / "build/artifact").write_text("ignored build output\n")
        self.assertEqual(self.checker.source_revision(self.log), revision)
        (self.repo / "shared.txt").write_text("uncommitted source work\n")
        with self.assertRaises(RuntimeError):
            self.checker.source_revision(self.log)
        self.assertEqual((self.repo / "shared.txt").read_text(), "uncommitted source work\n")
        self.git("restore", "shared.txt")
        (self.repo / "new-shader.txt").write_text("untracked source work\n")
        with self.assertRaises(RuntimeError):
            self.checker.source_revision(self.log)
        self.assertEqual((self.repo / "new-shader.txt").read_text(), "untracked source work\n")


if __name__ == "__main__":
    unittest.main()
