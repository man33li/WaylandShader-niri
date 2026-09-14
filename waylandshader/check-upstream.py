#!/usr/bin/env python3
"""Check committed fork merges against official niri releases, without promotion."""

import argparse
from datetime import datetime, timezone
import fcntl
from http.client import HTTPException
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen
import uuid


ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build"
CANDIDATES = WORK / "upstream-candidates"
API = "https://api.github.com/repos/niri-wm/niri"
UPSTREAM = "https://github.com/niri-wm/niri.git"



def command_environment():
    # A caller's GIT_DIR/INDEX_FILE/CONFIG_* must not redirect isolated operations.
    return {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}


def metadata(endpoint, log):
    url = API + endpoint
    log.write(f"GET {url}\n")
    log.flush()
    request = Request(url, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "WaylandShader-niri-upstream-check",
    })
    try:
        with urlopen(request, timeout=30) as response:
            payload = response.read(2 * 1024 * 1024 + 1)
    except HTTPError as error:
        detail = error.read(8192).decode("utf-8", errors="replace")
        raise RuntimeError(f"GitHub returned HTTP {error.code} for {url}: {detail}") from error
    if len(payload) > 2 * 1024 * 1024:
        raise RuntimeError(f"GitHub response exceeded 2 MiB: {url}")
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise RuntimeError(f"GitHub returned unexpected metadata: {url}")
    return data


def resolve(reference, log):
    revision = metadata("/commits/" + quote(reference, safe=""), log).get("sha")
    if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", revision):
        raise RuntimeError(f"GitHub did not resolve {reference!r} to a full commit SHA")
    return revision.lower()


def run(command, log, cwd=ROOT, allowed=(0,)):
    log.write("\n+ " + shlex.join(str(arg) for arg in command) + f"\nWorking directory: {cwd}\n")
    log.flush()
    completed = subprocess.run([str(arg) for arg in command], cwd=cwd,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               env=command_environment())
    output = completed.stdout.decode("utf-8", errors="replace")
    log.write(output)
    log.write(f"\nCommand exit status: {completed.returncode}\n")
    log.flush()
    if completed.returncode not in allowed:
        raise RuntimeError(f"Command exited with status {completed.returncode}; see log_path")
    return completed.returncode, output.strip()


def git(*args, log, cwd=ROOT, allowed=(0,)):
    # Hooks and automatic maintenance must not run user code or rewrite a candidate.
    return run(["git", "-c", "core.hooksPath=/dev/null", "-c", "maintenance.auto=false",
                "-c", "gc.auto=0", *args], log, cwd, allowed)


def safe_path(path):
    if path.resolve() != path:
        raise RuntimeError(f"Path must not be redirected by a symlink: {path}")


def write_report(path, result):
    safe_path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(result, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def source_revision(log):
    _, top = git("rev-parse", "--show-toplevel", log=log)
    if Path(top).resolve() != ROOT:
        raise RuntimeError("Checker must live in the root of the actual niri fork")
    _, shallow = git("rev-parse", "--is-shallow-repository", log=log)
    if shallow != "false":
        raise RuntimeError("Full fork and upstream history is required; unshallow the checkout first")
    _, revision = git("rev-parse", "--verify", "HEAD^{commit}", log=log)
    _, dirty = git("status", "--porcelain=v1", "--untracked-files=all",
                   "--ignore-submodules=none", log=log)
    if dirty:
        raise RuntimeError("Source has tracked or untracked changes; commit or move them before checking. "
                           "Ignored build output is permitted. See log_path for the source status.")
    # A merge can have an empty index diff; it still is not a committed source state.
    for marker in ("MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply"):
        _, location = git("rev-parse", "--git-path", marker, log=log)
        location = Path(location)
        if not location.is_absolute():
            location = ROOT / location
        if location.exists():
            raise RuntimeError(f"Source has an unfinished Git operation ({marker}); finish it first")
    return revision


def candidate_snapshot(candidate, log):
    _, head = git("rev-parse", "HEAD", log=log, cwd=candidate)
    _, unstaged = git("diff", "--name-only", "--ignore-submodules=none", log=log, cwd=candidate)
    _, untracked = git("ls-files", "--others", "--exclude-standard", log=log, cwd=candidate)
    _, unmerged = git("ls-files", "--unmerged", log=log, cwd=candidate)
    if unstaged or untracked or unmerged:
        raise RuntimeError(f"Candidate has edits, untracked sources or conflicts; retaining it untouched: {candidate}")
    _, tree = git("write-tree", log=log, cwd=candidate)
    code, merge_head = git("rev-parse", "--verify", "-q", "MERGE_HEAD", log=log,
                           cwd=candidate, allowed=(0, 1))
    return {"head": head, "tree": tree, "merge_head": merge_head if code == 0 else None}


def prepare_candidate(candidate, state_path, fork_revision, upstream_revision, log):
    safe_path(candidate)
    safe_path(state_path)
    if candidate.exists() or state_path.exists():
        if not candidate.is_dir() or not state_path.is_file():
            raise RuntimeError(f"Incomplete previous candidate retained; inspect it manually: {candidate}")
        state = json.loads(state_path.read_text())
        if (not isinstance(state, dict) or state.get("status") != "prepared"
                or state.get("fork_revision") != fork_revision
                or state.get("upstream_revision") != upstream_revision):
            raise RuntimeError(f"Previous failed/conflicted candidate retained; inspect it manually: {candidate}")
        _, top = git("rev-parse", "--show-toplevel", log=log, cwd=candidate)
        _, common = git("rev-parse", "--path-format=absolute", "--git-common-dir", log=log, cwd=candidate)
        _, original_common = git("rev-parse", "--path-format=absolute", "--git-common-dir", log=log)
        if Path(top).resolve() != candidate or common != original_common:
            raise RuntimeError(f"Candidate is not this fork's isolated worktree: {candidate}")
        if candidate_snapshot(candidate, log) != state.get("snapshot"):
            raise RuntimeError(f"Candidate index, HEAD or merge state changed; retaining it untouched: {candidate}")
        log.write("Reusing the unchanged prepared candidate; no reset or merge-abort performed.\n")
        return

    candidate.parent.mkdir(parents=True, exist_ok=True)
    state = {"fork_revision": fork_revision, "upstream_revision": upstream_revision, "status": "preparing"}
    write_report(state_path, state)
    git("worktree", "add", "--detach", candidate, fork_revision, log=log)
    try:
        git("-c", "user.name=WaylandShader upstream check", "-c", "user.email=upstream-check@localhost",
            "-c", "merge.autostash=false", "-c", "rerere.enabled=false", "-c", "commit.gpgsign=false",
            "merge", "--no-commit", "--no-ff", "--no-edit", upstream_revision, log=log, cwd=candidate)
        state["snapshot"] = candidate_snapshot(candidate, log)
        state["status"] = "prepared"
        write_report(state_path, state)
    except (OSError, RuntimeError, ValueError):
        state["status"] = "merge_failed"
        write_report(state_path, state)
        raise


def check(args, result, log):
    result["fork_revision"] = source_revision(log)
    result["stages"]["source"] = "passed"
    result["status"] = "lookup_failed"
    result["stages"]["lookup"] = "failed"
    reference = args.revision
    if reference is None:
        release = metadata("/releases/latest", log)
        if release.get("draft") is not False or release.get("prerelease") is not False:
            raise RuntimeError("GitHub's latest release is not a published stable release")
        reference = release.get("tag_name")
        if not isinstance(reference, str) or not reference:
            raise RuntimeError("GitHub's latest stable release has no tag")
        result["release_url"] = release.get("html_url")
    result["reference"] = reference
    revision = resolve(reference, log)
    result["upstream_revision"] = revision
    # Use the fixed official URL, never origin/upstream's configurable fetch URL.
    _, fetch_url = git("ls-remote", "--get-url", UPSTREAM, log=log)
    if fetch_url != UPSTREAM:
        raise RuntimeError("Git URL rewriting redirects the official upstream; remove that configuration before checking")
    git("fetch", "--no-tags", "--no-write-fetch-head", UPSTREAM, revision, log=log)
    _, fetched = git("rev-parse", "--verify", revision + "^{commit}", log=log)
    if fetched != revision:
        raise RuntimeError("Fetched upstream commit does not match resolved immutable revision")
    result["stages"]["lookup"] = "passed"
    fork_revision = result["fork_revision"]
    code, _ = git("merge-base", "--is-ancestor", revision, fork_revision, log=log, allowed=(0, 1))
    result["upstream_is_ancestor"] = code == 0
    # Refuse source changes that raced metadata/fetch before reporting or preparing.
    result["status"] = "source_failed"
    result["stages"]["source"] = "failed"
    if source_revision(log) != fork_revision:
        raise RuntimeError("Source HEAD changed during lookup; rerun against a stable committed checkout")
    result["stages"]["source"] = "passed"
    if args.revision is None and code == 0:
        result["status"] = "up_to_date"
        return

    result["status"] = "merge_failed"
    result["stages"]["merge"] = "failed"
    candidate = CANDIDATES / fork_revision / revision
    result["candidate_dir"] = str(candidate)
    state_dir = CANDIDATES / ".state" / fork_revision
    safe_path(candidate)
    safe_path(state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)
    lock_path = state_dir / (revision + ".lock")
    safe_path(lock_path)
    with lock_path.open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError(f"Another check is already using {candidate}") from error
        prepare_candidate(candidate, state_dir / (revision + ".json"), fork_revision, revision, log)
        result["stages"]["merge"] = "passed"
        result["status"] = "merge_passed"
        if args.build:
            result["status"] = "build_failed"
            result["stages"]["build"] = "failed"
            before_build = candidate_snapshot(candidate, log)
            builder = candidate / "waylandshader/build.py"
            if not builder.is_file() or builder.is_symlink():
                raise RuntimeError(f"Candidate has no regular waylandshader/build.py: {builder}")
            prefix = candidate / "build/niri-install"
            command = [sys.executable, builder, "--prefix", prefix, "--jobs", str(args.jobs)]
            # Stream long builds to disk rather than retaining their output in memory.
            log.write("\n+ " + shlex.join(str(arg) for arg in command) + f"\nWorking directory: {candidate}\n")
            log.flush()
            completed = subprocess.run([str(arg) for arg in command], cwd=candidate,
                                       stdout=log, stderr=subprocess.STDOUT,
                                       env=command_environment())
            log.write(f"\nCommand exit status: {completed.returncode}\n")
            log.flush()
            if completed.returncode:
                raise RuntimeError(f"Candidate builder exited with status {completed.returncode}; see log_path")
            if candidate_snapshot(candidate, log) != before_build:
                raise RuntimeError("Candidate builder changed tracked source or merge state; inspect the retained candidate")
            result["stages"]["build"] = "passed"
            result["status"] = "build_passed"
            result["install_prefix"] = str(prefix)


def report_path(value):
    path = Path(os.path.abspath(value.expanduser()))
    safe_path(path)
    if path.is_relative_to(ROOT):
        if not path.is_relative_to(WORK):
            raise ValueError("--report must not overwrite repository sources; use build/ or an external state directory")
        relative = path.relative_to(WORK)
        protected = {"upstream-candidates", "librashader-source", "niri-deps", "niri-support",
                     "niri-install", "niri-package"}
        if not relative.parts or relative.parts[0] in protected:
            raise ValueError("--report must not overwrite candidates, logs, dependencies or staging")
    if path.suffix != ".json":
        raise ValueError("--report must name a .json report file")
    if path.exists():
        if not path.is_file():
            raise ValueError("--report must be a regular file")
        try:
            existing = json.loads(path.read_text())
        except (ValueError, UnicodeError) as error:
            raise ValueError("Refusing to overwrite a file that is not a checker report") from error
        if (not isinstance(existing, dict) or "checked_at" not in existing or "stages" not in existing
                or not ("fork_revision" in existing or "pinned_revision" in existing)):
            raise ValueError("Refusing to overwrite a file that is not a checker report")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", help="Explicit official tag, branch or commit to test, even if already an ancestor")
    parser.add_argument("--build", action="store_true", help="Build and locally stage the merged candidate")
    parser.add_argument("--report", type=Path, help="Atomically write the JSON result to this file")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.revision is not None and (not args.revision.strip() or args.revision.startswith("-")):
        parser.error("--revision must be a nonempty official reference, not an option")
    try:
        report = report_path(args.report) if args.report else None
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))
    result = {
        "schema_version": 2,
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "fork_revision": None,
        "upstream_revision": None,
        "reference": args.revision,
        "status": "source_failed",
        "stages": {"source": "failed", "lookup": "not_run", "merge": "not_run", "build": "not_run", "runtime": "not_run"},
        "candidate_dir": None,
        "log_path": None,
        "runtime_validation": "Not performed: merge/build checks do not validate a GPU or native compositor session.",
    }
    log = None
    try:
        safe_path(CANDIDATES)
        logs = CANDIDATES / "logs"
        safe_path(logs)
        logs.mkdir(parents=True, exist_ok=True)
        log_path = logs / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex + ".log")
        log = log_path.open("x", encoding="utf-8")
        result["log_path"] = str(log_path)
        check(args, result, log)
    except (OSError, RuntimeError, ValueError, URLError, HTTPException) as error:
        result["error"] = str(error)
        if log is not None:
            try:
                log.write(f"\nERROR: {error}\n")
            except OSError as log_error:
                result["log_error"] = str(log_error)
    finally:
        if log is not None:
            try:
                log.close()
            except OSError as error:
                result["log_error"] = str(error)
    successful = (result["status"] in ("up_to_date", "merge_passed", "build_passed")
                  and "error" not in result and "log_error" not in result)
    if report is not None:
        try:
            write_report(report, result)
        except (OSError, RuntimeError) as error:
            result["report_error"] = str(error)
            successful = False
    print(json.dumps(result, indent=2))
    return 0 if successful else 1


if __name__ == "__main__":
    sys.exit(main())
