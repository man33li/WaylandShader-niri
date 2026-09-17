# WaylandShader maintenance toolkit

These tools, skills and rules capture lessons from the niri port, workspace
extraction, session regression and package verification. They are maintained in
this checkout, not installed as background services or global agent settings.

[Build and controls](README.md) · [Refactoring](REFACTORING.md) ·
[History](HISTORY.md) · [Upgrades and rollback](UPGRADING.md)

## The session fix this toolkit preserves

Commit `0178b8ac`, packaged as `26.04.ws0.2.2-2`, restores the stock session
pipeline under separately named WaylandShader launchers and units. The old
entry called the raw compositor with `--session`, bypassing login-shell setup
and graphical-session ownership.

That explained the two reported desktop failures:

- Noctalia's process lacked Flatpak export roots in `XDG_DATA_DIRS`, even though
  the user manager had them. Updating the manager cannot retroactively update a
  running shell's environment.
- The selected GNOME portal backend required `graphical-session.target`, which
  was inactive. A frontend advertising FileChooser did not mean its backend
  could activate.

The fix derives session assets from upstream resources, imports environment
before compositor startup, preserves READY/target ordering and cleans up the
session on exit. It does not add per-application paths, force a different portal
backend, overwrite stock niri, or globally enable more daemons.

The [verification record](HISTORY.md#managed-desktop-session-parity-022-2)
distinguishes actual Noctalia/chooser and private lifecycle checks from the
unperformed native Bottles umu, dinit and physical reboot qualification. The
AMDGPU DMUB messages also occurred before fork startup and around stock niri
transitions; this fix does not claim to resolve that driver issue.

## Tools

Run these commands from the repository root as the desktop user, not through
sudo. No new Python package dependencies are required by the maintenance CLIs.

| Tool | Purpose | Effect boundary |
| --- | --- | --- |
| `waylandshader/diagnose-session.py` | Current process/manager environment, session units, Flatpak exports and optional journal evidence | Read-only; no repairs or service activation |
| `waylandshader/verify-package.py` | Inspect this project's Arch package structure and managed-session contract | No extraction, execution or installation of payloads |
| `waylandshader/build.py` | Build this checkout and stage native support, compositor, controls and session assets | Writes the selected build/staging paths; no system installation |
| `waylandshader/run-nested.py` | Render preview on a private bus with disposable shader/XDG settings | Runs a nested compositor; does not validate native session lifecycle |
| `waylandshader/check-upstream.py` | Prepare/check a committed upstream merge candidate | Isolated candidate work; no promotion, installation or live restart |
| `waylandshader/watch-upstream.py` | Opt-in scheduled upstream checking | Creates only its owned maintenance units when requested |
| `build/niri-support/niri_bridge_test` | Retained real-GPU EGLImage/temporal/mipmap regression | Run explicitly on supported hardware; exit77 is a skip, not success |

### Read-only session evidence

```sh
python3 waylandshader/diagnose-session.py
python3 waylandshader/diagnose-session.py --journal --boot 0 --lines 80
python3 waylandshader/diagnose-session.py --journal --boot -1 --lines 80
```

Output is JSON with `schema_version: 1`. Default process selection is the current
user's niri, WaylandShader and native Noctalia processes. Repeat `--pid` to select
specific current, owned processes instead. Do not reuse a PID from an old
screenshot without checking its identity.

The tool allowlists environment fields rather than dumping tokens, passwords or
command lines. It separates process snapshots from manager environment and
reports missing commands, permission failures, disappearing processes and other
unavailable evidence. Advisory findings do not certify that an entire desktop
is healthy. Exit0 means a report was collected, exit2 means invalid arguments,
and a fatal collection failure exits1.

Important limits:

- `/proc/PID/environ` usually describes the initial exec environment. It is not
  proof of every later in-process environment change.
- Process/unit observations are **current**, even with `--boot -1`. Historical
  journal selection does not reconstruct a historical process environment.
- Journal collection is opt-in and bounded by `--lines` (1–500). It reports
  recognized signals and selected metadata, not raw messages or command lines.
  No visible record does not prove an event never occurred; direct compositor
  output may exist only on a TTY. Use scoped journalctl queries for exact messages
  or a wider timeline, and review those messages before sharing.
- Selected paths and session identifiers can still be personal information.
  Review reports before sharing; nothing is uploaded automatically.
- A portal's selected backend and the real application request still need to be
  traced. The tool does not open a chooser, launch Flatpaks or restart anything.

If saving evidence, keep it outside tracked source. For example:

```sh
mkdir -p build/diagnostics
python3 waylandshader/diagnose-session.py --journal \
  > build/diagnostics/session.json
```

### Inspect a package without installing it

```sh
python3 waylandshader/verify-package.py \
  waylandshader/niri-waylandshader-26.04.ws0.2.2-2-x86_64.pkg.tar.zst
```

Use an exact archive path, not a glob that can pick an older build. Output is
JSON with `schema_version: 1`, an `ok` result, errors, package identity/version
and the archive's SHA-256. Exit0 means the structural contract passed; exit1
means the artifact was invalid or unsupported; exit2 means usage was invalid.

The inspection covers required payloads and notices, ownership/modes, stock path
isolation, the managed login entry and meaningful service/target relationships.
Repeated systemd assignments must retain their meaning. Old raw-session packages
are expected to fail this newer managed-session contract.

This is intentionally a checker for this package, not a general package manager.
It accepts uncompressed tar and, with **Python 3.14+**, Zstandard-compressed tar.
Unsupported compression/runtime support fails explicitly; no dependencies are
installed automatically. Current limits are 512 MiB archive/expanded size,
64 KiB inspected text/header reads, 4096 members and a 64 MiB Zstandard window.
Only regular files/directories and the expected fork unit pair are supported;
links, special files and additional unit/drop-in definitions are rejected rather
than interpreted as a speculative installation filesystem.

It does **not** verify publisher signatures, MTREE digests, ABI behavior, GPU
correctness or legal compliance. An archive SHA is useful for comparing an
independently trusted expected digest; merely computing it does not authenticate
the publisher. License-file presence does not prove all corresponding-source
obligations were met. No packaged script or binary is run by this checker.

For a trusted locally built package, still exercise its staged binary without
build-tree linker overrides, check the private-library RUNPATH and perform the
relevant real rendering/session checks before a safe next-login installation.

## Skills and rules

Two repository-local skills use the [Agent Skills format](https://agentskills.io/specification):

| Skill | Use it for |
| --- | --- |
| `wayland-session-debugging` | Launcher/Flatpak/portal failures, environment drift, startup/shutdown and kernel-message attribution |
| `wayland-shader-integration` | Runtime/adapters, GL/FFI ownership, rendering defects, upstream merges and package qualification |

Their files live under `.agents/skills/<name>/SKILL.md`. Root `AGENTS.md` links
them and contains the durable project guardrails. Open this checkout as the
agent's workspace. Clients that discover `.agents/skills` can load the skills
directly; other clients can follow the explicit paths in `AGENTS.md`.

These skills depend on this checkout's tools and references. They are not
self-contained files to copy alone into an unrelated global skill directory.
No global harness configuration or third-party skill installation is performed.
For another compositor project, reuse the reasoning and safety boundaries but
adapt paths, service ownership and renderer contracts to that project.

## Reusable investigation loop

1. **Establish the boundary.** Record actual binaries, revisions, current process
   identity and the user's established symptom. Do not infer the running build
   from a directory name or the user manager's environment alone.
2. **Trace the complete path.** For desktop failures: login, profile, environment,
   compositor readiness, target dependencies, portal/backend and application.
   For rendering: element capture/draw, GPU exchange, temporal state and teardown.
3. **Compare meaningful evidence.** Separate stock/fork/greeter phases and boot
   versions. Normal SIGTERM and exit-time disconnects are not automatically
   regressions. Verify numeric driver status meanings against source.
4. **Fix the shared cause.** Reuse upstream session resources and existing
   renderer helpers. Preserve source ancestry and user state; avoid per-app,
   per-preset, portal-selection or kernel-flag workarounds without evidence.
5. **Prove the affected behavior.** Run the real thing. Keep regressions for
   plausible behavior/privacy/input-boundary failures, not source wording or
   mocked forwarding. An unavailable check is not a pass.
6. **Deliver with limits.** Record what ran, what did not, the source/package
   revision and rollback path. Clean up only owned helpers. Publish authorized
   source before distributing a corresponding binary; do not replace the live
   desktop to finish a task.

For full desktop/portal experiments, the existing rendering preview is not a
complete lifecycle sandbox. Use private HOME/runtime/config/cache/data/state,
private D-Bus **and activation environment**, unique test targets and verified
private input/IPC sockets. Unix socket paths must remain short enough. Confirm
readiness and cleanup; never substitute the live graphical target or enter real
credentials into a disposable helper prompt.

## Validation

The scripts follow the repository's stdlib unittest convention:

```sh
python3 -m unittest discover -s waylandshader/tests -p 'test_*.py'
```

Parser/privacy and malformed-package regressions are deterministic and do not
change services. They do not replace the [native verification gate](UPGRADING.md#verification-gate).
The package checker should also be exercised on an actual known-good archive and
an older known-broken raw-session archive when both are available.
