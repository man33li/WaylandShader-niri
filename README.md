# WaylandShader-niri

Native whole-output RetroArch Slang shaders and per-monitor color controls for
[niri](https://github.com/niri-wm/niri).

**Version 0.2.2: workspace-runtime edition.** Shader rendering and control state
live in the `waylandshader-runtime` Rust crate, connected to niri through small
compositor-local adapters. This is a separately built niri fork, not a plugin
for an unmodified compositor or a screen-capture overlay.

This version is developed on **`refactor/waylandshader-workspace`**. Publishing
that branch does not promote it to `main`; use the checkout instructions below.

## What WaylandShader adds

- **Native Slang presets:** run RetroArch `.slangp` chains with their passes,
  textures and declared parameters through a private librashader runtime.
- **Live controls:** a Qt GUI and asynchronous D-Bus CLI for loading presets,
  changing parameters and enabling or bypassing processing.
- **Per-monitor adjustments:** independent shader/color switches, gamma and
  saturation. Color adjustments also work without a shader preset.
- **Transactional loading:** compilation runs on a worker; a failed load keeps
  the working preset rather than replacing it with a broken chain.
- **Persistent recent presets:** the ten most recently loaded successful presets
  are shared by GUI and CLI and survive restarts.
- **Independent temporal history:** each output owns its frame history, while
  the selected preset and its parameters are shared across outputs.

Effects apply at final presentation. Source screenshots and screencasts remain
unfiltered; production rendering does not use CPU readback, a capture portal or
a PipeWire loop.

## Build and try it

Arch/CachyOS packaging and Linux source builds are maintained. Install the
[build dependencies](docs/waylandshader/README.md#build-from-source) first:
current stable Rust, a C++20 toolchain, CMake 3.25+, Qt 6.10+, Ninja, Python 3,
and the compositor/graphics development libraries listed there.

Clone with full history and build as your normal user:

```sh
git clone --branch refactor/waylandshader-workspace https://github.com/man33li/WaylandShader-niri.git
cd WaylandShader-niri
python3 waylandshader/build.py --jobs 4 --tests
python3 waylandshader/run-nested.py
```

The builder compiles this checkout, prepares the patched private librashader,
and stages executables under `build/niri-install/`. It does **not** install
system-wide or replace/restart the running compositor. `--tests` builds the GPU
regression executable; it does not run the hardware checks.

Run the preview from an existing Wayland desktop. It opens a nested compositor
and controller on a private D-Bus with disposable shader settings. Effects apply
only inside that window; `winit` is its output, not a physical monitor. Close
the window or press **Alt+Shift+E** to exit.

See the [full build and preview guide](docs/waylandshader/README.md) for native
dependencies, source-only sessions and troubleshooting boundaries.

## Controls

In a WaylandShader session, open `waylandshader-niri-controller`. Browse to a
preset, choose **Load preset**, then enable desktop effects. The **Recent** menu
fills the preset path; selecting an entry alone does not load it.

The CLI uses the same compositor-owned state:

```sh
waylandshader-nirictl outputs
waylandshader-nirictl load /absolute/path/to/preset.slangp
waylandshader-nirictl enable
waylandshader-nirictl output eDP-2 color on
waylandshader-nirictl output eDP-2 gamma 1.2
waylandshader-nirictl output eDP-2 saturation 0
waylandshader-nirictl disable
```

Replace `eDP-2` with an output returned by `outputs`. Presets are not bundled;
keep their shader files, textures and relative includes together. In the nested
preview, run commands from a terminal launched **inside** its private session,
for example `python3 waylandshader/run-nested.py -- alacritty` if installed.
An ordinary host terminal does not control that isolated preview.

Shader settings default to `~/.config/waylandshader/niri.json`. They are separate
from niri's compositor configuration. See [controls and settings](docs/waylandshader/README.md#controls-and-settings)
for parameter commands, output identity, overrides and CLI error semantics.

## Install alongside stock niri

After the [verification gate](docs/waylandshader/UPGRADING.md#verification-gate),
build an Arch package from the checkout:

```sh
cd waylandshader
makepkg
```

The current recipe is **`niri-waylandshader` 26.04.ws0.2.2-2**. It installs:

| Component | Name |
| --- | --- |
| Compositor | `niri-waylandshader` |
| Session launcher | `niri-waylandshader-session` |
| User compositor service | `niri-waylandshader.service` |
| Settings GUI | `waylandshader-niri-controller` |
| CLI | `waylandshader-nirictl` |
| Login session | **niri (WaylandShader)** |

Stock `niri`, its service and its login entry are not replaced. Updating the
distribution's `niri` package does not update this fork.
The login entry uses the session launcher, not the raw compositor command.
It restores stock niri's login-shell environment, graphical-session/portal
lifecycle and XDG autostarts with separate fork unit names. Do **not** enable
the compositor service globally or add duplicate portal/Noctalia autostarts.
See [managed startup and shutdown](docs/waylandshader/README.md#managed-session-startup-and-shutdown).

Save work and log out before changing the compositor package. Install the exact
archive from another session or a TTY, keeping a known-good package and the stock
login entry for rollback. Do not restart a compositor or display manager inside
your running desktop. Follow the [installation and rollback procedure](docs/waylandshader/UPGRADING.md#install-and-roll-back-without-replacing-a-live-compositor).

## Workspace-runtime architecture

| Location | Responsibility |
| --- | --- |
| `waylandshader/runtime/` | Rust shader manager, GLES element, FFI, settings, D-Bus and native linking |
| `src/waylandshader/` | Niri monitor identity, redraw registration and GLES/TTY renderer adapters |
| `src/backend/{tty,winit}.rs`, `src/niri.rs` | Presentation hooks, output/lock handling and GPU/backend lifetime |
| `waylandshader/bridge.{cpp,h}` | Desktop-GL runtime, EGLImage exchange and color processing |
| `waylandshader/client/` | Qt controller and CLI |
| `waylandshader/*.py` | Build, isolated preview and upstream-maintenance tools |

The runtime shares niri's pinned Smithay revision but does not depend on niri's
State, configuration crate or TTY renderer. Niri supplies the integration policy.
The crate is source-linked into the compositor; it is not a dynamically loaded
extension.

This boundary replaces the monolithic shader module, not the need to maintain
render hooks. An automatically reapplied sidecar patch would still need matching
niri source, rebuilding and lifecycle validation after upgrades. We retain
reviewed upstream merges instead.

Read the [refactoring process](docs/waylandshader/REFACTORING.md) for the original
coupling, extraction steps, API boundaries, preserved invariants and verification.

## Supported scope and limits

- **SDR sRGB/RGBA8**, not an HDR-preserving pipeline.
- Hardware desktop GL, EGLImage import/export and GPU fence support are required.
  Software renderers and differing render/scanout GPUs are rejected with bypass,
  not CPU-copy fallbacks.
- Active filtering disables hardware planes/direct scanout on that output.
  Animated presets schedule frames; no latency, power or VRR guarantee is made.
- Shader distortion changes pixels, not pointer hit regions.
- The 0.2.2 refactor passed 222 nonvisual Rust tests, independent runtime feature
  checks, native bridge regressions on Radeon 680M/RX 6700S, and real nested-session
  checks including VHSPro, rollback, resize and restart persistence.
- Those checks **do not certify physical DRM, lock security, hotplug, GPU removal,
  cross-GPU operation or HDR/VRR behavior**. Validate the intended native session
  before deployment; see the [recorded verification](docs/waylandshader/HISTORY.md#workspace-runtime-extraction-022).

## Documentation and contributions

- [Build, preview, packaging and controls](docs/waylandshader/README.md)
- [Refactoring process and crate boundaries](docs/waylandshader/REFACTORING.md)
- [Project history and verification records](docs/waylandshader/HISTORY.md)
- [Upstream upgrades and rollback](docs/waylandshader/UPGRADING.md)
- [Contributing to this fork](CONTRIBUTING.md)
- [WaylandShader-niri issues](https://github.com/man33li/WaylandShader-niri/issues)

For the underlying compositor's layout, keybindings and desktop setup, use
[niri's documentation](https://niri-wm.github.io/niri/) and
[configuration guide](https://niri-wm.github.io/niri/Configuration%3A-Introduction.html).
Stock-niri installation and release claims are not claims about this fork.
The historical KWin implementation lives in
[Wayland-Shader-KDE](https://github.com/man33li/Wayland-Shader-KDE), not a second
build mode here.

## Credits and licenses

Built on niri by Ivan Molodetskikh and contributors, with Slang processing through
librashader. The compositor and extracted Rust runtime retain
[GPL-3.0-or-later](LICENSE). Imported native WaylandShader support and controls
retain [MIT](waylandshader/LICENSE); the patched private librashader retains
MPL-2.0. Shader presets and other dependencies retain their own licenses.
