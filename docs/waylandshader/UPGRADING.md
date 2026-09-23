# Manual upstream upgrades and rollback

[Build and controls](README.md) · [Refactoring process](REFACTORING.md) · [History](HISTORY.md) ·
[Following Smithay](SMITHAY.md) · [Design decisions](DECISIONS.md)

The maintained repository is
[man33li/WaylandShader-niri](https://github.com/man33li/WaylandShader-niri).
`origin` is this fork; `upstream` is
[the official niri repository](https://github.com/niri-wm/niri).
The shared `main` keeps upstream ancestry through **merges**, not rebases,
force-pushes, a second source clone, or a refreshed niri patch.

**0.3.0 branch status:** the workspace-runtime line with multi-GPU presentation
was promoted to the shared `main` after native testing. Maintain it on `main`,
following the procedure below; `refactor/waylandshader-workspace` remains only as
the published history of the 0.2.2/0.3.0 development line.

**Nothing here automatically upgrades a running compositor.** A checker can
prepare/build a candidate, but a person resolves conflicts, validates it,
promotes the source, builds a package, and chooses when to change sessions.
Commands below assume a POSIX shell, the repository root and one shell session
so variables remain defined. Stop on an unexpected failure; do not blindly paste
past a conflict or failed check.

## Initial setup and publication

For a fresh clone, follow the [build guide](README.md#build-from-source). For an
existing clone, inspect its identity before changing anything:

```sh
git remote -v
git branch --show-current
git rev-parse --is-shallow-repository
git status --short
```

Expected URLs are `https://github.com/man33li/WaylandShader-niri.git` for origin
and `https://github.com/niri-wm/niri.git` for upstream. An authenticated SSH URL
for your own origin is also fine. Add upstream only if absent:

```sh
git remote add upstream https://github.com/niri-wm/niri.git
```

If shallow, fetch complete history before attempting merges/checks:

```sh
git fetch --unshallow origin
```

Publish reviewed commits by fast-forwarding `main`, as described in
[Commit, promote and publish](#commit-promote-and-publish):

```sh
git push origin main
```

Do not force-push to make a rejected push succeed. Fetch and inspect the remote
changes first. The old KWin checkout is not the new fork merely because its
folder was named WaylandShader-niri; check `git remote -v`.

## Prepare a recoverable baseline

Commit intentional source work or move unrelated files out of the checkout.
Do not auto-stash, discard changes, or run the checker on a dirty tree: it checks
a committed fork SHA, not uncommitted source. Finish/abort any previous Git
operation deliberately before starting another. Ignored build output is fine.

On clean fork `main`:

```sh
git switch main
git fetch origin
git merge --ff-only origin/main

timestamp=$(date -u +%Y%m%d-%H%M%S)
backup="$HOME/waylandshader-backups/$timestamp"
mkdir -m 700 -p "$backup"
git tag "waylandshader-before-$timestamp"
git bundle create "$backup/fork.bundle" --all
git rev-parse HEAD > "$backup/fork-revision.txt"
pacman -Q niri-waylandshader > "$backup/installed-package.txt"
```

On a source-only installation, omit the pacman query and record your staged
binary/version instead. A bundle preserves committed repository history, not
untracked files or installed binaries. Also keep the **exact known-good package
archive** outside this checkout. Do not assume the distribution cache or GitHub
contains a locally built package. Keep stock niri installed and know how to
reach its login entry or a TTY.

Back up shader settings and your normal niri config without putting them in Git:

```sh
config_home=${XDG_CONFIG_HOME:-$HOME/.config}
shader_config=${WAYLANDSHADER_CONFIG:-$config_home/waylandshader/niri.json}
printf '%s\n' "$shader_config" > "$backup/shader-settings-path.txt"
if [ -f "$shader_config" ]; then
    cp -a -- "$shader_config" "$backup/waylandshader-niri.json"
fi
if [ -d "$config_home/niri" ]; then
    cp -a -- "$config_home/niri" "$backup/niri-config"
fi
```

If the optional weekly checker is installed, disable it temporarily while doing
source maintenance:

```sh
python3 waylandshader/watch-upstream.py uninstall
```

This removes only its owned check units, not `niri.service`, and retains reports
and candidates. Reinstall it after committing/promoting the clean result.

## Fetch an immutable upstream target

For the main-based fork, normally use current official main:

```sh
git fetch upstream main
target=$(git rev-parse upstream/main^{commit})
printf '%s\n' "$target" > "$backup/upstream-target.txt"
git log --oneline HEAD.."$target"
git merge-base --is-ancestor "$target" HEAD
```

The final command returns **0** if this target is already included: no upstream
merge is needed. Return **1** means there are upstream commits to integrate;
other errors must be investigated. Review upstream release notes and changes,
especially Smithay, renderer, DRM/output lifecycle, lock and capture changes.
You can instead fetch a chosen official stable tag and resolve `target` from
that tag; use an exact SHA for the rest of the procedure so a moving branch
cannot change the target halfway through.

### Optional isolated preflight

From a clean, committed fork, these commands do not change `main`:

```sh
# Latest published official stable release; may already be included in main:
python3 waylandshader/check-upstream.py --report build/upstream-stable.json
# Current official main, with a separate build:
python3 waylandshader/check-upstream.py --revision main --build \
  --jobs 4 --report build/upstream-main.json
# Or check the immutable target resolved above:
python3 waylandshader/check-upstream.py --revision "$target" \
  --report build/upstream-target.json
```

The checker resolves official metadata, fetches from the fixed official URL,
and records full `fork_revision` and `upstream_revision`. It requires full
history and a clean source/index with no unfinished merge/rebase. An explicit
reference remains testable even when it is already an ancestor. Default stable
checks stop at `up_to_date` when that stable release is included; this does not
mean fork HEAD equals the stable tag or includes every newer main commit.

Candidates are detached worktrees of **the committed fork**, under:

```text
build/upstream-candidates/<fork-sha>/<upstream-sha>/
```

Only those candidates receive `git merge --no-commit --no-ff` and optional
`waylandshader/build.py` builds. Normal `build/niri-install` is not overwritten.
Reports distinguish source/lookup/merge/build failures, with `log_path` and
`candidate_dir`. Runtime is always `not_run`. Nonzero is failure, not successful
compatibility with a warning. Git URL rewriting that redirects the official
URL is refused. Reports must be `.json` files in a safe build/state location;
unrelated existing files, sources and protected build paths are not overwritten.

A conflicted, edited or manually resolved candidate is **retained**, not reset
or reused as though it were pristine. Inspect it with `git -C PATH status` and
read its log; save useful resolutions on a named branch before doing anything
else. The checker will refuse that same immutable pair until you deliberately
move the retained candidate aside. To keep the evidence and allow a clean retry:

```sh
# Set candidate to the exact candidate_dir from the report first.
git worktree move "$candidate" "$backup/retained-candidate"
```

Also move the matching preparation record out of
`build/upstream-candidates/.state/<fork-sha>/<upstream-sha>.json` into your backup;
do this only when no check for that pair is running. Do not delete the record
while its worktree still occupies the expected path. Prefer resolving the
actual upgrade on the named branch below instead of modifying preflight trees.

## Merge on an upgrade branch

Starting from clean `main` with a target not already included:

```sh
upgrade="upgrade/niri-$timestamp"
git switch -c "$upgrade"
git merge --no-ff --no-commit "$target"
```

A clean merge stops before committing. A conflict also stops: inspect and resolve
it, preserving both upstream behavior and shader invariants. Do not replace
entire source files with old fork copies or blindly select `--ours`/`--theirs`.

```sh
git status --short
git diff --name-only --diff-filter=U
```

Pay particular attention to:

- `src/backend/tty.rs` and `winit.rs`: final-presentation hooks, proper GPU choice,
  capture bypass, output damage/animation and disabling direct scanout only when
  processing is active. New upstream frame optimizations must not skip effects.
- `src/niri.rs`, `src/lib.rs`, `src/waylandshader/`: manager lifetime, output
  registration, monitor-profile identity, control redraw registration, the
  GLES/TTY render adapter and lock/history hooks. Release GL objects **before**
  their EGL display/backend disappears. Preserve test/no-D-Bus startup guards.
- `waylandshader/runtime/`: shader state, GLES presentation and the control
  service. Keep this crate independent of niri's State, output-name policy and
  TTY renderer. Its Smithay types must use the shared workspace revision.
- The runtime's `build.rs` owns native link env variables and libraries; root
  `build.rs` owns the executable RUNPATH and upstream probes. Preserve workspace
  membership, the root dependency and `dbus` feature forwarding. Keep private
  dependencies such as `parking_lot` in the runtime crate; do not downgrade
  Smithay to make old code compile.
- Packaging/workflows: upstream merges may reintroduce deleted recipes or stock
  install/release jobs. Keep the explicitly maintained Arch/source scope and
  distinct session/executable names; inspect modify/delete conflicts carefully.
- `resources/niri-session`, session units and `waylandshader/CMakeLists.txt`:
  preserve the generated fork namespace, login-shell/environment setup, READY
  ordering and shutdown target. Never replace the login entry with a bare
  `niri-waylandshader --session` or overwrite stock service files.

Resolve Cargo.toml first. For Cargo.lock conflicts, preserve the upstream locked
dependencies and reconcile the fork additions; if necessary use upstream's lock
as a deliberate baseline, then `cargo update -p niri` and inspect the lock diff.
Do not run an unrestricted `cargo update` just to silence merge conflicts.
The normal builder uses `--locked` and must accept the final committed lockfile.

The separate librashader pin/patch is **not** automatically upgraded when niri is
merged. If that dependency needs updating, treat it as a separate reviewed
change: port the lifetime, history and mip-exposure fixes, rebuild from a fresh
private dependency tree, and rerun both GL paths. Never remove the patch to make
a failed application disappear.

Smithay reaches the fork through niri's pin, so a merge can move it. Smithay is
used unmodified, but `gpu_copy()` in `src/waylandshader/mod.rs` repeats its
CPU-copy fallback checks: whenever the merge changes the Smithay revision,
follow the review in [SMITHAY.md](SMITHAY.md#review-what-changed). That guide
also covers the rare out-of-band Smithay update.

For each newly distributed upstream snapshot, increment `pkgrel` in
`waylandshader/PKGBUILD`. When the upstream release family or extension version
changes, update `pkgver` appropriately and reset `pkgrel` to 1; extension version
changes also update `waylandshader/CMakeLists.txt` and the runtime crate's
`waylandshader/runtime/Cargo.toml` version. Keep niri's own Cargo version aligned
with upstream rather than using it for cosmetic fork branding.

Record the upstream SHA, package version, integration adjustments, verification
and remaining limitations in [HISTORY.md](HISTORY.md). Review all staged/untracked
files before committing; never include personal presets/config, recordings,
packages, caches or handoff archives.

## Verification gate

Do this on the upgrade branch before promotion. The builder supports an
uncommitted merge so conflicts can be fixed and tested; the optional checker
requires a commit and is not the tool for this intermediate state.

```sh
python3 waylandshader/build.py --jobs 4 --tests

export WAYLANDSHADER_BRIDGE_LIB_DIR="$PWD/build/niri-support/lib"
export WAYLANDSHADER_RASHADER_LIB_DIR="$PWD/build/niri-deps/lib"
export LD_LIBRARY_PATH="$WAYLANDSHADER_RASHADER_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cargo test --locked --workspace --exclude niri-visual-tests --jobs 4
cargo check --locked --bin niri --no-default-features --jobs 4
cargo check --locked -p waylandshader-runtime --no-default-features --jobs 4
cargo check --locked -p waylandshader-runtime --no-default-features --features dbus --jobs 4
python3 -m unittest discover -s waylandshader/tests -p test_upstream.py
sh -n build/niri-install/bin/niri-waylandshader-session
systemd-analyze --user verify \
  build/niri-install/lib/systemd/user/niri-waylandshader.service \
  build/niri-install/lib/systemd/user/niri-waylandshader-shutdown.target

build/niri-support/niri_bridge_test waylandshader/tests/fixtures
MESA_GL_VERSION_OVERRIDE=3.3 build/niri-support/niri_bridge_test waylandshader/tests/fixtures
WAYLANDSHADER_TEST_RENDER_NODES='/dev/dri/renderD129 /dev/dri/renderD128' \
  cargo test --locked --lib --jobs 4 waylandshader::tests::cross_gpu_presentation -- --ignored
python3 waylandshader/run-nested.py
```

The Mesa override exercises the desktop-GL 3.3 path on Mesa; it is not a universal
driver option. On hybrid Mesa systems, repeat both GPU commands with `DRI_PRIME=1`
for the other supported GPU. Exit 77 / a CTest skip means the required GPU path
was unavailable, **not passed**. The retained GPU regression covers EGLImage
live mipmaps/base-level exposure, history/reset/resize, parameters, orientation,
color and border clamp. Readback occurs only in the test executables. On machines
with two GPUs, the ignored cross-GPU test presents the shader through Smithay's
transfer in both directions (8/10-bit targets, rotation, reflection, resize) and
fails if presentation damage stops reaching the target GPU; list your own render
nodes.

In the preview, inspect the actual GUI and exercise a real preset plus color-only
mode, enable/bypass, a parameter change, invalid-preset rollback and resize.
Confirm source captures remain unfiltered. Compare a representative temporal
preset over multiple frames, not just its first frame. A build or ordinary Rust
test does not validate GL interop. The hosted CI compiles the GPU regression but
does not certify hardware rendering.

Also check **Recent** after successful GUI and CLI loads: verify most-recent-first
ordering, no duplicate entries, explicit **Load preset** after selection, and
unchanged history after a failed load. Restart the controller to confirm it
reads the compositor's history. `run-nested.py` intentionally discards settings
when its compositor exits; test compositor-restart persistence with an isolated
saved `WAYLANDSHADER_CONFIG`, never by restarting the live desktop.

Before production adoption, validate on the intended physical outputs: ordinary
rendering, disable/reenable, lock/unlock, suspend/resume, resize/scale and
hotplug. Keep work saved and a known-good login path. Cross-GPU, HDR and VRR
support must not be inferred from a passing nested session; the shader pipeline
is SDR, and outputs whose GPU only receives CPU-copied frames stay unfiltered. On
hybrid machines, validate monitors on both GPUs together. Do not claim a
security or lifecycle check you did not perform.

On a systemd desktop, also validate the [managed session lifecycle](README.md#managed-session-startup-and-shutdown)
after a normal logout/login: `niri-waylandshader.service` and
`graphical-session.target` should be active, Flatpak applications should be
discoverable by the actual launcher, and a real portal chooser should open.
Check ordinary logout and the intended reboot/suspend paths separately. Do not
start another native compositor or restart portals under a running desktop to
perform this check. Kernel display-controller errors require their own
stock-versus-fork/boot comparison; a SIGTERM notice alone is not a crash.

## Commit, promote and publish

When verification and the history entry are complete, inspect the files, stage
the intended resolutions/changes, and finish the merge:

```sh
git status --short
git diff --check
git add -A
git commit -m "Merge upstream niri $target into WaylandShader"
git switch main
git merge --ff-only "$upgrade"
git push origin main
```

If `main` changed meanwhile, do not reset it or force promotion. Update the
upgrade branch against current fork main and repeat affected verification.
Rebuild/package the **committed** revision so its version metadata identifies
the promoted source. If you only consume a newer maintained fork revision, a
clean `git pull --ff-only origin main` followed by this build/verification/install
procedure is sufficient; you need not perform your own upstream merge.

## Install and roll back without replacing a live compositor

For source-only deployments, use the [versioned-prefix procedure](README.md#source-only-session).
Retain the previous complete prefix, select the new one only after leaving the
old session, and roll back by starting the retained prefix. Do not run pacman
commands for a source-only installation or overwrite an in-use prefix.

Build the Arch archive as the ordinary user:

```sh
cd waylandshader
makepkg
makepkg --packagelist
```

Copy the exact resulting archive to your retained-package directory. Do not
use a glob that might select several old/new packages. Save work, log out, and
from a TTY or another desktop run `sudo pacman -U` with that exact path. Then
select **niri (WaylandShader)**. Check `waylandshader-nirictl outputs`: physical
connectors, not just `winit`, indicate the actual desktop session.
The login entry must use `niri-waylandshader-session`; do not globally enable its
service or start it alongside stock niri. For logs use
`journalctl --user -b -u niri-waylandshader.service`.

If the new compositor cannot be used:

1. Log out normally if possible; otherwise use a TTY. Select the original stock
   **niri** entry for a working desktop. Do not restart a display manager or
   compositor service under unsaved applications.
2. Outside the fork session, run `sudo pacman -U` with the exact retained
   known-good `niri-waylandshader` archive, then log into its session again.
   Rolling back Git alone does not roll back an installed binary/library.
3. If necessary, restore the backed-up shader JSON to the path recorded in
   `shader-settings-path.txt`, and restore the niri config after inspecting the
   changes. Do so while the fork is stopped, so it cannot overwrite the restore.
   Releases before extension 0.2.1 reject the new `recent_presets` JSON field.
   When rolling back to one of those releases, restore the pre-upgrade shader
   JSON, or remove only `recent_presets` from a copy of the newer JSON before
   restoring it. Keep the original newer file and stop the fork before editing.
4. Preserve the failed branch, package version, logs and report. To rebuild old
   source without rewriting main, use a separate worktree at the
   `waylandshader-before-...` tag. The bundle can recover committed history if
   the original checkout is lost.

Before committing a failed in-progress merge, `git merge --abort` on its named
upgrade branch returns it to the pre-merge state; save wanted resolutions first.
After publication, correct/revert the bad change in a new reviewed commit rather
than force-resetting shared `main`.

## Optional weekly checks and checkout migration

On clean committed source, without sudo:

```sh
python3 waylandshader/watch-upstream.py install
python3 waylandshader/watch-upstream.py status
```

Units are `waylandshader-niri-upstream.service` and `.timer`, **not niri.service**.
They check the latest official stable release weekly, with low CPU priority and
up to 30 minutes of randomized delay, building only when a candidate is needed.
They do not track every main commit, auto-merge shared history, publish, install,
restart a compositor or notify the display manager. A persistent timer can run
a missed check when the user manager next becomes available.

The report defaults to
`$XDG_STATE_HOME/waylandshader/niri-upstream.json` or
`~/.local/state/waylandshader/niri-upstream.json`. Inspect the report and journal:

```sh
systemctl --user start waylandshader-niri-upstream.service
journalctl --user -u waylandshader-niri-upstream.service -n 80 --no-pager
```

Installed units capture this checkout's absolute path and build-tool PATH.
When moving from the historical checkout or relocating this one, uninstall the
old owned units first, then install from the new checkout. For the historical
repository the command is `python3 niri/watch-upstream.py uninstall`; for this
fork it is `python3 waylandshader/watch-upstream.py uninstall`. Installation
refuses unrelated, customized, redirected or differently configured units; do
not overwrite them by hand to bypass the ownership check. Keep the checkout in
place while scheduled checks reference it. Reports and retained candidates
survive uninstall.
