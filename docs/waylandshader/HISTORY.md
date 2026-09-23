# History and maintenance decisions

[Build and controls](README.md) · [Manual upgrades](UPGRADING.md)

This records the project's development, including the standalone-fork migration
on **2026-09-14** and subsequent releases. Verification is scoped to each entry,
not a claim that every backend was re-tested for every change. Dates are
included where recorded; the exact date of the initial KWin implementation is
not inferred from file timestamps.

## Origin: native WaylandShader for KWin

The original [Wayland-Shader-KDE repository](https://github.com/man33li/Wayland-Shader-KDE)
implemented a native KWin 6.6 C++20 effect, a Qt settings application, and an
asynchronous D-Bus/CLI interface. RetroArch `.slangp` presets ran through
librashader 0.10.1 inside the compositor, not a screen-capture overlay.

The design established transactional worker-thread preset compilation,
per-output temporal history, shader parameters, persistent monitor profiles,
and independently enabled gamma/saturation. A private librashader build fixed
GL resource lifetimes and temporal initialization without replacing the system
library. Its private SONAME is retained here.

The inherited KWin record described a Nix build and three CTest checks, including
an isolated two-output KWin 6.6.6 session on an AMD RX 6800 XT. That is historical
evidence, not a full KWin rebuild on the later CachyOS laptop.

## Native niri integration: the separate-backend phase

The implementation was extended on the old repository's `niri-backend` branch;
its inherited repository baseline was commit
`6579b80e3cc7dd04ae3fd04f74c27fca5c8136bd`.
By 2026-09-13 it built a separately named niri from the v26.04 commit
`8ed0da44d974c32c6877d2f4630c314da0717ecb` using a maintained source patch.
KWin's root CMake/Nix build remained separate from the niri builder.

niri/Smithay renders with GLES, whereas the chosen shader runtime needs desktop
OpenGL. The port used **EGLImage storage exchange between unshared contexts**,
not unsupported cross-API share groups. It added GPU fence ordering, exact EGL
state restoration, per-output chain/history ownership, presentation-only hooks,
lock/history invalidation, and teardown before the backend EGL display vanished.

The port retained native Qt/CLI controls with distinct executable, D-Bus, settings
and login-session names. Stock niri and system librashader were not replaced.
Active filtering disabled hardware planes/direct scanout. Capture paths stayed
unfiltered; software rendering and incompatible cross-GPU paths were rejected.

Recorded CachyOS nested verification covered multipass/LUT rendering, live
parameters, failed-preset rollback, color and per-output bypass, resize,
persistence and immediate control acknowledgements. The GUI was displayed and
CLI/D-Bus mutations exercised. Pixel comparisons distinguished filtered
presentation from unfiltered source screenshots. These checks did not certify
native DRM/lock/hotplug/HDR/VRR behavior.

The user subsequently installed the separately named compositor as the actual
desktop session and reported a real VHSPro rendering defect. That user report
was not dismissed because the isolated tests had passed.

## 2026-09-14: VHSPro horizontal-band correction

The bridge allocated full mip storage but exposed uninitialized higher levels.
VHSPro's quantized coordinates could select those levels through implicit
texture derivatives even with scanlines/noise disabled. The result was dark
horizontal banding, not an intended parameter setting.

The fix is in the host, not a special-case shader edit:

1. New input images expose only their initialized base level.
2. Private GL 3.3 and GL 4.6 runtime paths expose additional levels when actually
   generating mipmaps requested by the preset.
3. A retained high-LOD/no-mipmap regression guards the undefined-level boundary.

The regression failed before the correction (zero instead of the expected
0.75 sample) and passed afterward. Isolated 121-frame real VHSPro renders
removed the defect on Radeon 680M and RX 6700S, Mesa 26.2.2. Default GL and
GL 3.3 bridge regressions passed on both GPUs; GL 3.3/default-GL frame 60 was
pixel-identical on the 680M. Separate shared-context runtime checks passed on
GL 4.6 and 3.3, including robust contexts; this was not a full KWin build.

The corrected historical Arch package was
`niri-waylandshader-26.04.ws0.1.0-2-x86_64.pkg.tar.zst`, SHA-256:

```text
165dc4c2ada4e5732548bad486c967fed83ed1e4f3815dbf8a7ad5b1a634e39d
```

That local artifact and the earlier package were retained in the old workspace
for rollback, not copied into Git. User presets/settings were not changed.
An unidentified GNOME reference build was unavailable for an exact parity
comparison; no pixel-parity claim is made.

## 2026-09-14: upstream-checking investigation

Official niri v26.04 and `main` at
`e1d3b0c47ce5bb77f16e5006aba604d23b233649` had no supported persistent
whole-output shader plugin interface. Animation shaders were not a replacement.
A `.so` would still require compositor hooks, so it would not eliminate upstream
integration work. [Upstream discussion #913](https://github.com/niri-wm/niri/issues/913)
provides context; this is a statement about the inspected baseline, not a
prediction that niri will never add an extension API.

The first checker tested pinned-patch candidates and optionally built them,
without installation or promotion. Stable v26.04 matched the pin. An explicit
pinned candidate built, while main failed the old Cargo.toml/Cargo.lock patch
contexts. Invalid references and an actual compiler failure produced distinct
failure reports. An owned current-user weekly timer was installed and exercised.

This exposed the maintenance problem: an independent niri patch in a KWin
repository discarded Git's useful fork ancestry and made ordinary upstream
changes look like patch-context failures.

## 2026-09-14: standalone WaylandShader-niri fork

The user created [man33li/WaylandShader-niri](https://github.com/man33li/WaylandShader-niri)
as a real fork of [niri-wm/niri](https://github.com/niri-wm/niri), with default
branch `main`. Development moved to a complete-history clone based directly on
upstream `e1d3b0c47ce5bb77f16e5006aba604d23b233649`.

The standalone migration:

- Integrates the Rust output manager and presentation/lifecycle hooks directly
  into niri's current source, retaining its current Smithay revision
  `22571baa20d34d71092942dbb520c4e3fbbd6263` rather than downgrading to v26.04.
- Builds the root Cargo workspace. Removes the second niri checkout, niri pin,
  module-copy step and maintained niri patch from the active build path.
- Keeps private librashader pinned/patched separately, including the VHSPro fix.
- Moves bridge, controls, build/package/preview tools and regressions under
  `waylandshader/`. Qt clients become niri-only; no KWin conditional build remains.
- Preserves executable/package identities, D-Bus protocol and niri settings path,
  so the repository move does not require resetting user shader profiles.
- Replaces patch applicability with committed-fork ancestry and isolated Git
  merge candidates. A dirty source tree is refused rather than silently checking
  an older HEAD. No checker result claims runtime validation.
- Retains an optional owned weekly report/build timer, but makes upstream
  promotion, installation and session changes explicitly manual.
- Uses extension version 0.2.0 / initial Arch package `26.04.ws0.2.0-1`.

The user explicitly selected **Arch and source builds**. Inherited Nix, RPM,
DEB and release/deployment entrypoints were retired instead of pretending bare
upstream builds would link the native bridge or install the separate session.
CI now builds this fork and its Qt support, runs nonvisual Rust tests and checks
the no-default-features build. GPU/native-session acceptance remains manual.
Upstream configuration/reference documentation and attribution remain intact.

The old workspace, KWin source, packages and private handoff archives were
preserved, not reset, removed or published. This migration does not install over
or restart the live compositor, and does not publish a remote commit by itself.

### Local migration verification

The checked-out main-based source, not a separately patched niri clone, built
and staged successfully on CachyOS x86_64 with Qt 6.11.2 and Mesa 26.2.2.
Verification during the migration included:

- 222 passing nonvisual Rust tests/doc-tests across niri, niri-config and
  niri-ipc; the no-default-features build also passed.
- The retained bridge regression on Radeon 680M and RX 6700S, through both
  default desktop GL and forced GL 3.3: four hardware passes, not skips.
- A private nested session displaying the migrated Qt controller, exercising
  real CLI/D-Bus color controls, a real VHSPro preset, a parameter change,
  failed-preset rollback, independent shader bypass and master disable.
- Matched 1244×1502 captures: the unfiltered source retained colored pixels;
  saturation-zero presentation had none and reflected the gamma adjustment.
- Normal nested compositor exit after a quit request. The live desktop's
  shader-settings checksum remained unchanged.
- Four real-Git fixture regressions for combining fork/upstream changes,
  retaining conflicts and manual resolutions, preserving staged candidate edits,
  and refusing dirty source while allowing ignored build artifacts. Moving a
  retained worktree/preparation record aside and retrying was exercised.
- The actual checker refused the uncommitted migration before upstream lookup,
  rather than reporting compatibility for the previous HEAD.

Hosted GitHub CI was configured, not executed or published by these local
checks. Physical DRM/lock/hotplug/HDR/VRR acceptance remains unclaimed. Current
official-ref preflight results and build logs are retained outside Git under
`build/upstream-candidates/`; the checker never labels those runtime passes.

## Recently used presets (0.2.1)

The controller adds a **Recent** menu next to the preset browser. Full paths
distinguish equal filenames; choosing an entry fills the path without applying
it until **Load preset** is used.

The compositor owns one shared, persistent list for GUI and CLI loads. It
records only successful preset commits, deduplicates reused paths, keeps the
latest ten entries, and preserves history through failed loads and unloads.
The existing shader JSON gains `recent_presets`; status exposes `recentPresets`.
Older settings are accepted and acquire history after the saved preset loads
successfully. Older binaries need their earlier settings restored on rollback;
see [UPGRADING.md](UPGRADING.md#install-and-roll-back-without-replacing-a-live-compositor).

Local verification used the real nested compositor and Qt controller:

- Release source build and the no-default-features check passed.
- Twelve distinct real shader loads verified the ten-entry limit and ordering.
- A symlink alias and repeated loads verified deduplication; a failed compilation
  preserved the working shader and history.
- The actual popup was captured and operated using input confined to the nested
  Wayland socket. Same-named files in different directories, spaces and an
  ampersand remained distinguishable and selectable.
- Selecting an entry did not load it; the load button applied it and promoted
  it in history. A deleted recent file produced the normal GUI error without
  replacing the working shader.
- History survived shader unload, controller replacement and a compositor
  restart. An older JSON without history retained its preset/output settings
  and acquired history after successful startup compilation.
- The previews exited normally; the live shader-settings checksum was unchanged.

These checks add no physical DRM, lock, hotplug, HDR or VRR certification.

## Workspace runtime extraction (0.2.2)

Implemented on `refactor/waylandshader-workspace`, preserving the recent-presets
work in checkpoint `d35070b6`.
The extraction itself is commit `aa5b8455`. The
[refactoring process](REFACTORING.md) records its migration sequence and API
boundaries; this section records the resulting behavior and verification.

- Moves shader state, the GLES element, FFI, settings and D-Bus into the
  `waylandshader-runtime` workspace crate under `waylandshader/runtime/`.
- Leaves monitor identity, event-loop registration and GLES/TTY adaptation in
  niri. Output, presentation, capture, lock and GPU teardown hooks stay local.
- Moves native link ownership into the crate. The final executable retains its
  private-library RUNPATH; the normal builder and private C ABI are unchanged.
- Shares niri's Smithay revision and forwards the root D-Bus feature. CI checks
  the runtime alone both with and without D-Bus, rather than relying only on
  niri's larger dependency feature set.
- Keeps makepkg's `waylandshader/src/` staging area separate from Rust sources.
  Existing package archives and extracted package directories are preserved.

A self-repatching sidecar was not adopted: it would still need niri source hooks,
recompilation and render/lifecycle validation after upstream updates. The crate
is a source boundary, not a plugin ABI for stock niri. Normal reviewed fork merges
remain the upgrade path.

Local verification:

- Release build, 222 nonvisual Rust tests and four upstream-maintenance fixtures
  passed. The runtime's two D-Bus feature configurations and niri's no-default-
  features build checked successfully.
- Native EGLImage regressions passed on Radeon 680M and RX 6700S, with default
  desktop GL and the Mesa GL 3.3 override.
- The real nested compositor rendered a constant shader at RGB `(38, 140, 217)`
  and grayscale at `(147, 147, 147)`, while source screenshots stayed unfiltered.
  Failed compilation retained the working shader, pixels and recent history.
- Color-only mode, output shader bypass and master bypass worked. Real VHSPro
  exposed 59 parameters; changing film grain changed animated frames. Rendering
  remained active after resizing from 1244×1502 to 900×1502.
- Preset, recent history, output controls and the changed parameter survived a
  compositor restart. Both isolated runs exited normally.

These checks do not certify physical DRM, lock security, hotplug, GPU removal,
cross-GPU operation, HDR, VRR, latency or power behavior. No system installation,
live-compositor restart, upstream merge or remote push was performed.

## Managed desktop session parity (0.2.2-2)

Implemented in commit `0178b8ac`. The [maintenance toolkit](MAINTENANCE.md) turns
the diagnosis and verification workflow into reusable commands and agent guidance.

The user reported missing Flatpak entries in native Noctalia 5.1.0 and no Bottles
installer file chooser, while both worked in stock niri. The packaged login
entry ran the compositor directly instead of using stock niri's session
launcher. This was a session-integration regression, not an established shader
rendering defect.

- The live Noctalia process lacked `XDG_DATA_DIRS`, despite correct Flatpak paths
  in the user manager. Stock login-shell initialization supplies those paths.
- `graphical-session.target` was inactive, and GNOME portal activation repeatedly
  failed its `Requisite` dependency. The backend started successfully during
  the intervening stock niri session.
- Direct compositor output went to the greeter's TTY, not `niri.service`'s
  journal. The separately named managed service now provides a useful log unit.

The package now derives a fork-named session launcher, systemd service/shutdown
target and dinit resources from upstream's checked-out resources. It retains
the login environment, readiness ordering, XDG autostarts and shutdown behavior.
Stock units and desktop identity remain unchanged. It refuses overlapping stock
or fork services and extends staging-path containment checks to the new unit
directories. No compositor/rendering code or kernel parameters changed.

Verification used temporary assets and isolated runtime/D-Bus namespaces:

- The installed launcher entered the real fish login environment; the resulting
  desktop catalog included system Bottles and user RetroArch exports. Manager
  calls in this environment probe were intercepted, never sent to the live bus.
- Generated shell syntax and systemd unit verification passed. The generic
  desktop-file validator rejects `DesktopNames` on both this entry and the stock
  niri session entry; that existing session-specific key was retained.
- An actual nested compositor ran under private copies of the generated systemd
  graph. The real GNOME backend failed its prerequisite before the private
  graphical target existed, then started after compositor readiness.
- Actual Noctalia displayed Bottles in its launcher. The real GNOME/Nautilus
  file chooser opened and returned a successful cancellation response without
  selecting a file.
- Target shutdown stopped the portal before compositor SIGTERM; niri exited 0.
  Noctalia's native logout command also exited the compositor 0. Its portal
  reported a lost Wayland connection, matching the stock logout pattern.
- Temporary units, helpers and runtime state were removed. The original live
  compositor and Noctalia processes were not restarted.

The non-session nested test does not expose Mutter's session service channel.
This is not certification of Bottles' exact native umu/transient-parent flow,
dinit operation, physical logout, suspend or reboot. A fresh native login is
required after installing the corrected package.

The supplied shutdown photos showed an older `7f91449f-modified` build receiving
SIGTERM, followed by Radeon 680M (`07:00.0`) DMUB errors. Retained logs show the
same error class on kernels `7.2.4-3-cachyos` and `7.2.5-1-cachyos`, before the
first fork launch and during stock niri startup/exit. On the photographed reboot,
the login scope stopped at 19:50:29 while kernel diagnostics continued to
19:50:50 (+04:00). The error is not unique to this fork.

In the [upstream 7.2.5 DMUB status definition](https://github.com/gregkh/linux/blob/v7.2.5/drivers/gpu/drm/amd/display/dmub/dmub_srv.h),
status 2 is a display-microcontroller command queue being full, not a shader
compiler error. Static inspection found no demonstrated shader-before-backend
teardown inversion. The session fix does not claim to cure this kernel/firmware
problem, and no driver reset or boot-parameter workaround was applied.

## Published build and HDMI diagnosis (0.2.2-3)

This package revision rebuilds the current workspace-runtime branch, including
the managed-session fix and maintenance documentation. It preserves the previous
`0.2.2-2` archive instead of overwriting a known-good build. Renderer behavior
and the private shader ABI are unchanged.

Read-only inspection of a reported HDMI-A-1 bypass found:

- The preset was loaded successfully, and HDMI shader/color controls were enabled.
- Status reported different target/render GPUs, with both HDMI processing paths
  inactive.
- The primary renderer was Radeon 680M at PCI `07:00.0` (`renderD129`, eDP-2).
  HDMI-A-1 belonged to the discrete Radeon at PCI `03:00.0` (`renderD128`).
- The TTY adapter's GPU equality check and runtime eligibility condition account
  for the bypass. It was not a missing preset or failed compilation.

The existing niri render-device option was syntax-validated for an HDMI-focused
next login, but no user configuration or live GPU selection was changed.
Selecting the other GPU changes which outputs are eligible and can increase
power use; this is not simultaneous cross-GPU support. The guard was retained.

The GitHub prerelease includes matching fork and patched-librashader source,
checksums and explicit verification/limitation notes. A source snapshot or
successful build does not establish native HDMI, HDR, cross-GPU or reboot
qualification.

## Cross-GPU presentation and render GPU preference

Implements phases A–D of the [multi-GPU plan](GPU-PLAN.md) on the workspace
runtime branch; no package was built or installed and the running desktop was not
touched.

- The TTY adapter now draws the shader output through `MultiFrame`, so the
  presentation damage reaches a monitor on another GPU. Previously it drew on the
  inner GLES frame; untracked damage could leave another GPU's monitor stale, which
  is why cross-GPU outputs were blocked.
- Smithay stays unmodified. Before an output on another GPU renders, niri repeats
  with public APIs the allocation and import checks Smithay makes before it falls
  back to CPU copies, and caches the answer until the output's mode, format or GPU
  changes. GPU copies are processed; CPU-copy outputs stay unfiltered with a
  reported reason and without animation redraws. The render-GPU equality guard
  was removed.
- The controller reports each monitor's route, the render GPU in use, startup
  fallback and the next-login preference, and can save the render GPU through an
  explicitly included `waylandshader-gpu.kdl` with validation and rollback.

Verification on Radeon 680M + RX 6700S is recorded in the plan's
[implementation status](GPU-PLAN.md#implementation-status). Physical HDMI scanout,
a real CPU-copy fallback, hotplug, performance and power remain for native
qualification before release.

## Going forward

Follow [UPGRADING.md](UPGRADING.md), not the historical pinned-patch procedure.
Keep the fork's shared `main` merge-based; preserve upstream changes and make
small integration adjustments. Record the exact upstream SHA, fork SHA, package
version, verification results and remaining hardware limitations for each
promoted upgrade. A clean merge or successful compilation is not evidence that
a new compositor is safe to replace a running desktop.
