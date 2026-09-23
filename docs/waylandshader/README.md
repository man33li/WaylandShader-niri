# WaylandShader-niri build and control guide

Native whole-output RetroArch Slang shaders and per-monitor color controls in a
standalone [niri fork](https://github.com/man33li/WaylandShader-niri).
This guide covers the **0.2.2 workspace-runtime edition**.

[Refactoring process](REFACTORING.md) · [Project history](HISTORY.md) ·
[Manual upstream upgrades and rollback](UPGRADING.md) · [Maintenance toolkit](MAINTENANCE.md)

## Repository and supported scope

- The 0.2.2 source line is `refactor/waylandshader-workspace`; publishing that
  branch does not promote it to `main`. The fork was initially based on official
  niri `main` at `e1d3b0c47ce5bb77f16e5006aba604d23b233649` (2026-09-14).
- The root Cargo workspace **is the compositor source**. There is no separate
  fetched niri tree, pinned `NIRI_REV`, or maintained niri patch to apply.
- Shader state, GLES rendering and controls live in the source-linked
  `waylandshader-runtime` workspace crate. Small niri-local adapters connect
  monitor identity, redraw events and the physical/nested renderers.
- Arch/CachyOS packaging and standalone Linux source builds are maintained.
  The inherited Nix/RPM/DEB recipes and upstream release/deployment automation
  were retired rather than left pointing at incompatible stock-niri builds.
- KWin is a separate historical backend in
  [Wayland-Shader-KDE](https://github.com/man33li/Wayland-Shader-KDE), not a build
  mode of this fork. Its `niri-backend` branch is historical/rollback material.
- One preset and its parameters are shared across outputs. Each output has
  independent temporal frame history and separate shader/color enablement, gamma
  and saturation.

Stock niri and this fork can remain installed together. The executable/package
name stays `niri-waylandshader`; the repository's name is `WaylandShader-niri`.
Updating the distribution's `niri` package does **not** update this fork.

## Build from source

Clone with complete history; the upgrade checker deliberately refuses shallow
checkouts:

```sh
git clone --branch refactor/waylandshader-workspace https://github.com/man33li/WaylandShader-niri.git
cd WaylandShader-niri
git remote add upstream https://github.com/niri-wm/niri.git
git fetch upstream main
```

On an up-to-date Arch/CachyOS system, install the dependencies as the user:

```sh
sudo pacman -S --needed base-devel git rust cmake ninja python pkgconf clang \
  cairo glib2 libdisplay-info libinput libpipewire libxkbcommon mesa \
  pango pixman seatd systemd-libs wayland libepoxy qt6-base
```

Use current stable Rust, C++20, CMake 3.25+, Qt 6.10+, Ninja, Python 3 and Git.
`waylandshader/PKGBUILD` is the package dependency list. Portals and
xwayland-satellite remain normal niri desktop setup choices, not shader
transport dependencies. Other distributions need equivalent development
headers and libraries; their package recipes are not maintained here.

Build as the normal user, **without sudo**:

```sh
python3 waylandshader/build.py --jobs 4 --tests
```

This builds the checked-out compositor and stages:

```text
build/niri-install/
  bin/niri-waylandshader
  bin/niri-waylandshader-session
  bin/waylandshader-niri-controller
  bin/waylandshader-nirictl
  lib/waylandshader/libwaylandshader-rashader.so.2
  lib/systemd/user/niri-waylandshader.service
  lib/systemd/user/niri-waylandshader-shutdown.target
  lib/dinit.d/user/{niri-waylandshader,niri-waylandshader.target}
  share/wayland-sessions/niri-waylandshader.desktop
  share/applications/org.waylandshader.NiriController.desktop
  share/doc/niri-waylandshader/{README,REFACTORING,HISTORY,UPGRADING,MAINTENANCE}.md
  share/licenses/niri-waylandshader/
```

The builder still fetches **librashader**, pinned at
`a910bee8d2ead0acf2f83b3e5ad0b8f8f66b53db` (0.10.1), and applies
`waylandshader/patches/librashader-gl-lifetime.patch`. Its private SONAME avoids
replacing the system librashader. Shader source, native support and compositor
build products live in `build/librashader-source`, `build/niri-support` and root
`target`, respectively. Existing mismatched dependency checkouts are refused,
not reset. Preserve any edits before moving an obsolete build tree aside.

`--tests` **builds**, but does not run, the hardware regression. `--support-only`
prepares the private library, bridge and Qt controls without compiling/installing
the compositor. `--prefix /usr` requires an explicit `--destdir` staging folder;
this script is not a privileged system installer.

### Isolated preview

Run from an existing Wayland desktop:

```sh
python3 waylandshader/run-nested.py
# Optional terminal inside the preview, if installed:
python3 waylandshader/run-nested.py -- alacritty
```

This opens a nested compositor and settings GUI on a private D-Bus, with
temporary config/cache/data/state directories and shader settings. Only its
window is filtered; the output named `winit` is not a physical monitor. Close
the preview or use **Alt+Shift+E**. Temporary settings are discarded on exit.
An extra application started with `--` inherits the isolated bus and staged
executables in `PATH`. Run control commands in that terminal, not an unrelated
terminal on the host bus.

### Arch package and actual desktop

After the verification in [UPGRADING.md](UPGRADING.md#verification-gate):

```sh
cd waylandshader
makepkg
```

Run `makepkg` as the ordinary user. It stages the package at
`build/niri-package/usr` and writes the package archive under `waylandshader/`.
The current package is `26.04.ws0.2.2-3`; `26.04.ws0.2.0-1` was the first
standalone integration. These versions do not claim the source equals the
v26.04 tag; record the Git SHA when distributing a build.
The recipe does not provide, conflict with, or replace stock `niri`.

Save work and log out normally before changing the compositor package/session.
From another session or a TTY, install the exact new archive with `sudo pacman -U`.
Choose **niri (WaylandShader)** in the display manager, keeping the original
**niri** entry available for rollback. The entry runs
`niri-waylandshader-session`, which uses its own compositor service and shutdown
target while preserving stock niri's session lifecycle. It uses your normal
niri configuration and does not replace `niri.service`.
Never restart the display manager or either compositor service inside your
running desktop. Re-enter the session after installing this packaging fix;
existing compositor/Noctalia processes do not acquire a new login environment.

### Source-only session

Without an Arch package, build into a **versioned local prefix** and retain the
previous prefix for rollback:

```sh
revision=$(git rev-parse --short=12 HEAD)
prefix="$PWD/build/releases/$revision"
python3 waylandshader/build.py --prefix "$prefix"
```

After validation, save work and log out. From a real TTY login, set `prefix` to
the already-built versioned directory. On systemd, link its two session units
into the user manager before launching:

```sh
systemctl --user link \
  "$prefix/lib/systemd/user/niri-waylandshader.service" \
  "$prefix/lib/systemd/user/niri-waylandshader-shutdown.target"
export PATH="$prefix/bin:$PATH"
unset WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY NIRI_SOCKET
"$prefix/bin/niri-waylandshader-session"
```

Do not run this inside another desktop. These user-unit links override packaged
unit definitions. When changing prefixes, inspect and replace only your existing
links while the fork is stopped; do not overwrite custom units. Remove those
links when returning to packaged units. The service embeds the configured
prefix's executable path, so configure/build for the intended prefix rather than
relocating an existing installation with `cmake --install --prefix`.

No display-manager entry is installed into system directories by local staging.
Never rebuild into an in-use prefix. Rollback means leaving the session and
selecting the retained prefix and its units, not replacing a running process's
files. Dinit resources are also derived from upstream; a dinit deployment needs
the matching niri build feature and service search-path setup. The session
verification recorded here exercised systemd, not dinit.

## Managed session startup and shutdown

The compositor binary's `--session` flag is **not** a substitute for niri's
session launcher. Before package revision `0.2.2-2`, the fork entry bypassed that
launcher. On the reported greetd setup this skipped Flatpak's login-shell data
paths and left `graphical-session.target` inactive, preventing the preferred
GNOME portal backend from starting.

The corrected systemd path is:

```text
display manager / TTY
  -> niri-waylandshader-session
     -> user's login shell and its profile/vendor environment
     -> import login environment into user systemd and D-Bus activation
     -> start and wait for niri-waylandshader.service
        -> graphical-session-pre.target
        -> niri-waylandshader --session
           -> backend, Wayland/IPC/X11 sockets and session D-Bus interfaces
           -> export display variables, then notify READY
        -> graphical-session.target and xdg-desktop-autostart.target
```

The launcher, units and dinit resources are generated from the checked-out
upstream `resources/` files, rather than maintaining a second launcher
implementation. Fork unit names are separate; desktop identity remains `niri`.
The launcher refuses an already-active stock or fork compositor service. Both
sessions still share a user manager, portal names and graphical targets, so
separate names do not make concurrent graphical sessions safe.

| Component | Owner and activation |
| --- | --- |
| Compositor service | Started explicitly by the selected session launcher; no global `enable` step |
| Graphical session and XDG autostarts | Pulled in and ordered by the compositor service; not manually started |
| GNOME/GTK portal backends | D-Bus-activated user services; GNOME requires an active graphical session |
| Noctalia and polkit agent | Existing niri configuration or the user's chosen autostart mechanism; do not add duplicate launchers |
| Xwayland-satellite | Niri's built-in on-demand X11 socket activation; do not spawn a second copy |
| PipeWire/WirePlumber | Existing user sockets/services and their dependencies, not compositor-specific copies |
| Keyring | Existing PAM/socket/D-Bus configuration; the fork does not create or automatically unlock a keyring |
| Power actions | Noctalia/niri clients of logind and system services, not direct GPU reset commands |

On compositor quit, the launcher waits for its service to finish, starts the
fork's shutdown target to stop the shared graphical targets, then clears the
display/session variables it imported. Services with `PartOf=graphical-session.target`
are stopped with the session. Detached application scopes are not all explicitly
bound to that target; do not assume every background application is killed.

During an ordered target stop, reverse ordering stops the portal backend before
the compositor. On direct niri/Noctalia logout the compositor can disconnect
Wayland first, so a portal's `Lost connection to Wayland compositor` or broken
pipe at that point can also occur with stock niri. Noctalia's native niri logout
uses niri IPC; reboot/poweroff instead hand off to systemctl/logind/PID 1.
`quitting due to receiving signal SIGTERM` is normal graceful-exit logging.

In the inspected Noctalia 5 setup, niri starts Noctalia and the KDE polkit agent.
The native shell reads TOML and its state-sidecar settings, not legacy
`settings.json`; do not change legacy idle/power settings expecting them to
control the native shell. Do not blindly copy a distro template enabling another
polkit agent while retaining the existing one.

For subsequent diagnostics, use the fork's actual unit:

```sh
systemctl --user status niri-waylandshader.service graphical-session.target
journalctl --user -b -u niri-waylandshader.service \
  -u xdg-desktop-portal.service -u xdg-desktop-portal-gnome.service
```

For a privacy-limited, read-only report from the source checkout, run
`python3 waylandshader/diagnose-session.py`. Optional `--journal --boot 0`
adds bounded diagnostic signals without raw journal messages. See the
[maintenance toolkit](MAINTENANCE.md) for package inspection, skills, rules,
report privacy and the distinction between current snapshots and older logs.

Kernel `amdgpu`/`DMUB` errors are a separate layer. In the reported machine's
retained logs they occurred before WaylandShader startup and around stock niri
transitions as well. Do not hide them or apply speculative driver flags as a
session fix; see the [session-parity verification record](HISTORY.md#managed-desktop-session-parity-022-2).

## Controls and settings

Run `waylandshader-niri-controller`, or use the CLI in the fork's session:

```sh
waylandshader-nirictl status
waylandshader-nirictl outputs
waylandshader-nirictl parameters
waylandshader-nirictl load /absolute/path/to/preset.slangp
waylandshader-nirictl enable
waylandshader-nirictl param PARAMETER_NAME 0.5
waylandshader-nirictl output eDP-2 shader on
waylandshader-nirictl output eDP-2 color on
waylandshader-nirictl output eDP-2 gamma 1.2
waylandshader-nirictl output eDP-2 saturation 0
waylandshader-nirictl disable
```

Replace `eDP-2` with an actual ID/name from `outputs` (`winit` in the preview),
and use a parameter declared by your preset. Selectors match exact ID first,
otherwise a unique exact name. Keep presets' relative includes, textures and
shader files together; presets are not bundled.

Use **Recent** beside **Browse…** to choose a recently used shader, then click
**Load preset** (or press Enter in the path field). Selection alone does not
replace the working shader. The menu shows full paths, most recent first, to
distinguish presets with the same filename.

The compositor remembers the last **10 successfully loaded presets**, including
loads made through the CLI. Loading an existing entry moves it to the top;
symlinked load paths resolve to the same entry. Failed loads do not add or
reorder entries, and unloading a shader does not clear the list. A moved or
deleted file remains listed and produces the normal file error when loaded;
use **Browse…** to locate its new path.

History survives controller and compositor restarts in the existing shader
JSON's `recent_presets` field; `waylandshader-nirictl status` exposes it as
`recentPresets`. Older settings files need no manual conversion: their saved
preset enters history after it loads successfully. History requires the updated
compositor, not just the updated controller; the menu is disabled when empty
or when connected to an older compositor without history support.

The master switch bypasses both processing paths without discarding settings.
Shader and color switches are independent; color works without a preset. Gamma
is 0.1–5.0 (1 neutral); saturation is 0–2 (1 neutral). Gamma operates on encoded
sRGB, then saturation on linear-sRGB Rec.709 luminance, after preset processing.
Neutral color settings do not require a color pass. Per-output controls do not
implicitly enable the master switch.

Preset compilation is asynchronous and replacement is transactional: a failed
load preserves the working preset. The CLI waits for compilation; `--timeout`
sets a 1–3600 second deadline. Exit codes are 0 success, 1 rejected/shader error,
2 usage, 3 unavailable/transport/protocol, 4 timeout. A timeout does not cancel
an accepted operation. Requested settings can be acknowledged before the next
frame updates the rendering-active flags.

- Shader settings: `$XDG_CONFIG_HOME/waylandshader/niri.json`, default
  `~/.config/waylandshader/niri.json`. `WAYLANDSHADER_CONFIG` selects an absolute
  alternate file; the preview uses a disposable one.
- Output profiles use niri's monitor identity when a serial is available,
  otherwise the connector. They are not portable KWin UUID profiles.
- D-Bus: service `org.waylandshader.Niri`, object `/WaylandShader`, interface
  `org.waylandshader.Effect`. The interface name is retained for protocol
  compatibility; it does not load a KWin plugin.
- `WAYLANDSHADER_DBUS_SERVICE` overrides the name for **both** compositor and
  clients. Clients never start/restart a compositor to make it available.

## Architecture and integration boundaries

```text
niri GLES scene
  -> RGBA8 EGLImage storage
  -> unshared desktop OpenGL context: Slang, gamma, saturation
  -> output EGLImage storage
  -> niri GLES final presentation
```

EGLImage siblings share GPU storage, not a GLES/desktop-GL context share group.
GPU fences/server waits order both directions; the prior EGL API, context and
surfaces are restored. Production rendering has no CPU readback, capture portal,
PipeWire loop or overlay window. Each output owns its temporal state. Resizing
and lock transitions invalidate history; lock presentation bypasses filtering.
The hooks affect final presentation, not capture paths. Active filtering disables
DRM hardware planes/direct scanout on that output and schedules animated frames.

The input initially exposes only initialized mip level zero. The private runtime
exposes/generates higher levels only when requested by the preset. This preserves
the VHSPro banding correction described in [HISTORY.md](HISTORY.md).

| Location | Responsibility |
| --- | --- |
| `waylandshader/runtime/` | Rust workspace crate: output manager, GLES element, FFI, settings, D-Bus and native linking |
| `src/waylandshader/` | Niri monitor-profile policy, event-loop registration and GLES/TTY element adapter |
| `src/backend/{tty,winit}.rs` | Physical/nested presentation and GPU lifecycle hooks |
| `src/niri.rs`, `src/lib.rs` | Manager lifetime, output/lock hooks and module registration |
| `build.rs`, root Cargo files | Executable RUNPATH, upstream build probes and workspace/dependency integration |
| `waylandshader/bridge.{cpp,h}` | Desktop-GL runtime, EGLImage exchange and color pass |
| `waylandshader/client/` | Niri-only Qt GUI and CLI, no KWin build branch |
| `waylandshader/patches/` | Private librashader fixes, not a niri patch |
| `waylandshader/{build,run-nested,check-upstream,watch-upstream}.py` | Build/preview/maintenance tools |

The crate shares niri's pinned Smithay revision, but does not depend on niri,
`niri-config` or its TTY renderer. Niri supplies a profile resolver when an output
is registered and a callback attaching the control worker's redraw source.
The local render-element wrapper delegates capture and draw without a heap
allocation. The root `dbus` feature enables the crate's optional D-Bus service.
The normal builder prepares the native libraries before Cargo; `--support-only`
prints the environment needed to check or build the crate independently.

The crate lives in `waylandshader/runtime/`, not `waylandshader/src/`: makepkg
owns the latter staging directory and may clear it. Build and package commands
are otherwise unchanged.

This is **SDR sRGB/RGBA8**, not HDR-preserving. Hardware desktop GL, EGLImage
import/export and fence support are required. Software renderers and differing
render/scanout GPUs are rejected with errors and bypass, not CPU-copy fallbacks.
Nested success is not certification of physical DRM, lock security, hotplug,
GPU removal, cross-GPU operation, HDR, VRR, latency or power behavior. These need
native-session validation on the intended machine before deployment.

niri does not expose a supported persistent whole-output shader plugin API at
the initial upstream baseline; see
[upstream discussion #913](https://github.com/niri-wm/niri/issues/913).
This crate is linked into the compositor, not loaded into an unmodified niri.
A self-patching sidecar would still need matching niri source, hook updates,
a rebuild and validation after each upgrade. It cannot patch an installed
binary into supporting shaders or make a clean patch prove rendering safety.
This fork therefore retains reviewed upstream merges rather than package-manager
auto-patching. The crate narrows the integration boundary; it does not eliminate
Smithay/render-lifecycle maintenance or turn this into a universal Wayland plugin.

## HDMI and hybrid-GPU outputs

niri composes every output on one render GPU, chosen at login. Effects run on
that GPU, before Smithay transfers each frame to the GPU that drives the monitor.
A monitor on another GPU is processed when that transfer is a GPU copy of a
shared dma-buf. When Smithay can only transfer through CPU copies, the monitor
stays unfiltered, because reading back every animated frame would be too
expensive, and status reports:

```text
Only CPU copies reach this monitor's GPU
```

niri decides this with the same allocation and import checks Smithay makes before
it falls back to CPU copies, when the monitor first renders and whenever its mode
or format changes, never every frame.

Check the actual routing before changing a preset:

```sh
waylandshader-nirictl status
journalctl --user -b -u niri-waylandshader.service \
  --grep='using as the render node|connecting connector'
```

Each entry of `outputs` reports `transfer` (`same-gpu`, `gpu-copy`, `cpu-copy`;
empty in nested sessions) and a nonempty `bypass` reason when it is unfiltered.
On the native session, `gpu` reports the render GPU in use, the GPU driving each
output, and the GPU configured for the next login.

### Choosing the render GPU

Automatic selection is the default; either GPU can then serve monitors on both.
The controller's **Render GPU** section can save a different GPU for the next
login (for example, a discrete GPU for demanding presets, at a power cost). It
writes only a file that you adopt explicitly, by adding this line at the end of
the niri configuration file in use (the controller shows its path):

```kdl
include optional=true "waylandshader-gpu.kdl"
```

The controller never edits that configuration file. It writes
`waylandshader-gpu.kdl` next to it atomically with a stable
`/dev/dri/by-path` device path, reloads the configuration to confirm the choice
takes effect, and restores the previous file when another part of the
configuration overrides it. It refuses to replace a hand-edited, read-only or
symlinked managed file. Automatic selection cannot clear a
`render-drm-device` set elsewhere: niri merges `debug` settings across includes
and an absent value does not reset an earlier one.

GPU selection happens during compositor initialization. The running session
keeps its GPU until a normal logout/login; do not restart the compositor live.
If the saved GPU is missing at login, niri selects automatically and the
controller says so.

Cross-GPU presentation passed a real-GPU regression on render nodes in both
directions (Radeon 680M and RX 6700S; 8- and 10-bit targets, rotation,
reflection and resize). Physical HDMI scanout, a real CPU-copy fallback,
hotplug, performance and power require the native qualification in the
[multi-GPU plan](GPU-PLAN.md#e-native-qualification-and-release-gate).

## Licenses

niri and the extracted Rust runtime retain GPL-3.0-or-later (the runtime inherits
the root Cargo workspace license). Imported native WaylandShader support and
controls retain MIT (`waylandshader/LICENSE`); the patched private librashader
retains MPL-2.0.
The combined compositor is distributed under niri's GPL obligations. Shader
presets and other dependencies retain their own licenses.

### Binary release source

The [v26.04.ws0.2.2-3 prerelease](https://github.com/man33li/WaylandShader-niri/releases/tag/v26.04.ws0.2.2-3)
provides the package, checksums and a matching source bundle. The bundle contains
the tagged fork snapshot and the full patched librashader source, with provenance
in `SOURCE-INFO.txt`. Librashader is based on
`a910bee8d2ead0acf2f83b3e5ad0b8f8f66b53db`; the fork's committed
`waylandshader/patches/librashader-gl-lifetime.patch` supplies its modifications.
Those covered sources and modifications remain available under MPL-2.0; the
surrounding support directory's MIT license does not relicense librashader.
Upstream also offers GPL-3.0-only, as recorded in its crate manifests.

For the normal Git-based rebuild path, clone this release tag and use the build
commands above:

```sh
git clone --branch v26.04.ws0.2.2-3 https://github.com/man33li/WaylandShader-niri.git
```

Cargo dependencies and toolchains are still obtained through the documented
build process and locked manifests. The source bundle is not a vendored,
offline/hermetic build environment or a certification of every dependency's
license obligations.
