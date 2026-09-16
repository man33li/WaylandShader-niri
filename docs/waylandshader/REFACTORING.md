# Refactoring WaylandShader into a workspace runtime

This documents the **0.2.2 workspace-runtime extraction**, implemented on
`refactor/waylandshader-workspace`. Recent-presets work was preserved in
`d35070b6`; the extraction is commit `aa5b8455`.

[Build and controls](README.md) · [Verification record](HISTORY.md#workspace-runtime-extraction-022) ·
[Upstream upgrades and rollback](UPGRADING.md)

## Goal and decision

Separate shader implementation from niri-specific integration without changing
what users see: preset loading, rendering, controls, recent history and settings
must continue to work with the existing executables and protocol.

The alternatives considered were:

| Approach | Maintenance consequence |
| --- | --- |
| Sidecar that reapplies a niri patch after updates | Still needs matching source, patch conflict handling, a rebuild and semantic render/lifecycle validation. It cannot add a supported extension API to stock niri. |
| Keep the monolithic in-tree shader module | Works, but couples shader implementation directly to niri's State, monitor-name policy and physical renderer types. |
| Workspace runtime plus local adapters | Keeps the existing fork and its Git ancestry while making compositor-specific policy and rendering adaptation explicit. This is the implemented approach. |

A successful patch application or compilation is not proof that new upstream
render scheduling, damage handling or teardown semantics are safe. Reviewed
upstream merges remain necessary; no package-manager auto-patching was added.

The new crate is independently selectable **within the Cargo workspace**. It
still requires Smithay, native graphics libraries and the normal native-support
build. It is not a published, self-contained SDK, dynamic Rust plugin or a way
to load shaders into an unmodified installed niri.

## Starting point

Before extraction, `src/waylandshader/mod.rs` contained the shader manager,
per-output runtime state, GLES element and niri TTY renderer implementation.
Its neighboring `control.rs` owned settings, persistence and D-Bus, but startup
took niri's `State`. `bridge.rs` wrapped the private native C ABI. Root
`build.rs` owned the shader libraries' linker inputs alongside niri's build probe.

The native EGLImage/desktop-GL implementation already lived under
`waylandshader/`. The refactor did not rewrite that bridge, replace librashader,
change the private SONAME, or introduce a new rendering pipeline.

## Extraction sequence

### 1. Preserve the working behavior

Create a dedicated architecture branch and checkpoint the existing recent-presets
feature before moving code. Retain its persistent ten-entry history, success-only
commit semantics and GUI/CLI behavior. Preserve package artifacts and user data;
do not install over or restart the live compositor during development.

The extraction used the existing fork as its baseline. It did not merge a newer
upstream niri snapshot into the same change, keeping dependency updates separate
from the architecture change.

### 2. Move implementation into a real crate

Add `waylandshader/runtime` to the root Cargo workspace as
`waylandshader-runtime` version `0.2.2`, with `publish = false`.

| Previous location | Result |
| --- | --- |
| `src/waylandshader/mod.rs` | Shader implementation moves to `waylandshader/runtime/src/lib.rs`; the old path becomes a small niri adapter module. |
| `src/waylandshader/control.rs` | Moves to `waylandshader/runtime/src/control.rs`, with host event-loop registration supplied by the caller. |
| `src/waylandshader/bridge.rs` | Moves unchanged to `waylandshader/runtime/src/bridge.rs`. |
| Root native shader linking | Moves to `waylandshader/runtime/build.rs`. |

Use `waylandshader/runtime/src/`, **not** `waylandshader/src/`. The latter is
makepkg's source-staging directory and may be cleared during package builds.
The Rust crate retains the root workspace's GPL-3.0-or-later license; moving it
beneath the native support directory does not change it to MIT.

There are no compatibility reexports of the old manager/control modules. Niri
now refers directly to `waylandshader_runtime::Manager`.

### 3. Move monitor identity policy out of the runtime

`Manager::new` accepts a `fn(&smithay::output::Output) -> String` profile resolver.
Niri implements that resolver using its existing `niri_config::OutputName`
policy: serial-bearing monitor identity when available, connector name otherwise.

The resolver runs only when an output runtime is created. This includes both
explicit `add_output` registration and the existing lazy-registration path in
`prepare`. It is not called on every frame, and extraction does not change the
settings keys used to reconnect a monitor to its saved profile.

The runtime continues to own per-output shader state and history; it no longer
imports niri's configuration types to decide how an output should be named.

### 4. Inject event-loop registration, not niri State

`Control::start` accepts a host callback with the contract
`FnOnce(calloop::ping::PingSource) -> Result<(), String>`.

The runtime creates its redraw ping and asks the host to register the source
**before** starting the control worker. Niri's adapter inserts it into niri's
existing event loop and calls `queue_redraw_all` when it fires. The runtime keeps
the original startup guard, worker/channel ownership and error reporting.

This removes the control module's dependency on niri `State` without adding a
second event loop or changing D-Bus names, configuration paths, persistence,
request acknowledgements or transactional preset replacement.

### 5. Keep renderer adaptation local

The crate's `ShaderElement` implements Smithay's `Element` and
`RenderElement<GlesRenderer>`. It does not know niri's TTY renderer or its
`AsGlesFrame` helper.

Niri defines a local wrapper through Smithay's existing `render_elements!`
macro, then retains the TTY-to-GLES capture/draw delegation on that local type.
This satisfies Rust's trait ownership rules without introducing a crate
cycle, trait-object rendering or a heap allocation for the wrapper.

Both the physical and nested backend insertion points wrap the core element.
The wrapper must preserve **all** element semantics, especially
`is_framebuffer_effect`, damage/commit information, framebuffer capture and draw.
Losing the capture flag or callback would change rendering even if the code
still compiled.

### 6. Move link ownership and preserve feature boundaries

The runtime's build script owns the existing native inputs:

- `WAYLANDSHADER_BRIDGE_LIB_DIR` for the static C++ bridge;
- `WAYLANDSHADER_RASHADER_LIB_DIR` for the private librashader;
- link directives for the bridge, `waylandshader-rashader`, epoxy and the C++
  standard library, including rebuild tracking for the native artifacts.

Root `build.rs` retains niri's libinput probe and the executable-specific
`$ORIGIN/../lib/waylandshader` RUNPATH. `waylandshader/build.py` still prepares
native support before building the compositor. Neither the C ABI nor the
installed executable/library names changed.

The crate uses the workspace's pinned Smithay revision with `renderer_gl`.
Shared calloop and zbus versions come from workspace dependencies. The root
compositor no longer owns the runtime's private `parking_lot` dependency.

The runtime defaults to no features; its optional `dbus` feature enables its
service. Niri's existing `dbus` feature explicitly forwards to
`waylandshader-runtime/dbus`. There is no new shader on/off build feature.
Independent crate checks ensure niri's larger Smithay feature set does not hide
a missing dependency in the extracted library.

## Invariants kept during the move

| Boundary | Required behavior |
| --- | --- |
| Presentation vs capture | Only final output presentation is filtered; source screenshots and screencasts remain unfiltered. |
| Loading | Worker compilation and transactional replacement retain the working preset on failure, without adding failed loads to recent history. |
| Output state | Temporal history remains independent per output. Geometry/context changes and lock transitions invalidate it; lock presentation bypasses filtering. |
| Scheduling | Active filtering disables hardware planes/direct scanout for that output; animated shaders continue requesting frames. |
| GPU ownership | Preserve GPU fence ordering, EGL state restoration, and texture/EGLImage/runtime lifetimes. Release shader resources before backend EGL display destruction. |
| Teardown | Keep output removal, TTY pause, GPU removal and compositor-drop hooks in niri. Worker joins remain lifecycle teardown work, not frame-path work. |
| Unsupported paths | Keep software-renderer/cross-GPU errors and bypass; do not add a CPU-copy fallback. |
| User contract | Preserve executable names, D-Bus interface, settings schema, monitor profile policy and recent-preset behavior. |

The graphics path is still GLES scene → EGLImage storage → unshared desktop-GL
Slang/color processing → output EGLImage storage → GLES presentation. The crate
boundary does not add a capture process, context share group or pixel readback.

## How the result was verified

Verification combined independent compilation with real rendering, rather than
treating a file move or successful build as sufficient:

1. Build and stage the release compositor through the normal builder.
2. Check the runtime alone with and without D-Bus, and check niri with no default
   features. Run the existing nonvisual Rust and upstream-maintenance suites.
3. Run the native EGLImage regression on Radeon 680M and RX 6700S using both
   default desktop GL and Mesa's GL 3.3 override.
4. Launch the actual compositor and Qt controller on an isolated nested Wayland
   session/private D-Bus. Compare filtered presentation with unfiltered source
   captures, exercise invalid-preset rollback, color-only mode and bypass.
5. Load real VHSPro, change a live parameter, observe animated frames, resize,
   then restart and verify persisted preset/history/output controls/parameters.
6. Build the separately named package and check the executable's private-library
   RUNPATH and loading without `LD_LIBRARY_PATH`.

The result included **222 passing nonvisual Rust tests**, four maintenance
fixtures and the real-GPU/nested checks recorded in
[HISTORY.md](HISTORY.md#workspace-runtime-extraction-022). During packaging,
makepkg exited successfully but emitted a libfakeroot diagnostic; archive
ownership, payload hashes and MTREE checksums were checked separately.

CI checks the standalone runtime feature combinations. Hosted CI builds the GPU
regression but does not certify real hardware rendering. Physical DRM, lock
security, hotplug, GPU removal, cross-GPU operation, HDR, VRR, latency and power
behavior remain outside the recorded certification. This documentation update
does not claim a new run of those compositor tests.

## Maintaining this boundary

Keep niri-specific policy in the adapter rather than importing niri back into
the runtime crate. Share the Smithay revision; do not add a second copy merely
to make an upstream API change compile. Review both the adapter and the core
when framebuffer-effect, damage, scheduling or EGL lifetime contracts change.

Use the [upstream upgrade procedure](UPGRADING.md) for reviewed merges, dependency
reconciliation, the runnable verification commands and rollback. A small adapter
makes integration easier to locate; it does not make upgrades automatic or
remove the need for native-session testing.
