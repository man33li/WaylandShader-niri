---
name: wayland-shader-integration
description: Maintain source-linked Wayland compositor shader integrations, including niri workspace boundaries, GLES/desktop-GL interop, EGLImage and GPU lifetime, preset rollback, capture semantics, upstream merges, packaging and evidence-backed releases. Use for renderer or FFI changes, dependency upgrades, native rendering defects, and package qualification.
compatibility: This niri/WaylandShader source checkout, Rust/C++ native build dependencies and supported real GPU access for rendering checks. Package inspection itself is read-only.
---

# Wayland shader integration

Preserve behavior at the compositor boundary before optimizing or moving code.
Prefer existing Smithay elements, workspace types, build tools and upstream
session resources over a new plugin framework or maintained copy of niri.

## Map the boundary before editing

| Area | Existing ownership |
| --- | --- |
| Shader manager, GLES element, settings and control worker | `waylandshader/runtime/src/` |
| Monitor identity, redraw registration, GLES/TTY adaptation | `src/waylandshader/` |
| Final presentation and output/GPU/lock hooks | `src/backend/{tty,winit}.rs`, `src/niri.rs` |
| Native desktop-GL runtime and image exchange | `waylandshader/bridge.{cpp,h}` |
| Dependency pin and private fixes | `waylandshader/build.py`, `waylandshader/patches/` |
| Login/service lifecycle | `resources/`, adapted by `waylandshader/CMakeLists.txt` |

Trace every caller and teardown path. Shared Rust types must use the same pinned
Smithay revision. Keep niri policy out of the runtime crate: profile resolution
runs on output creation, and the host registers the control worker's redraw
source. Preserve feature forwarding and check the crate independently, not only
through niri's larger feature set.

## Keep the renderer contracts

- GLES and desktop OpenGL contexts are unshared. Exchange EGLImage storage,
  restore EGL bindings, and preserve GPU fence/server-wait ordering in both
  directions. Do not assume OpenGL/GLES share groups work.
- Keep production rendering on the GPU. Do not introduce a capture portal,
  PipeWire feedback loop, CPU readback or per-frame `glFinish` to hide a bug.
- Imported textures, EGLImages, native chains, contexts and displays have
  different lifetimes. Trace shared ownership and EGL sibling rules. Join
  compilation and retire native resources while the backend/display still live.
- Never expose uninitialized input mip levels. Preserve initialized base-level
  sampling unless the preset requests generated mipmaps; fix the shared host
  path rather than special-casing a problematic shader.
- Keep transactional replacement and success-only recent history. Failed
  compilation must leave the working preset and history intact.
- Keep per-output temporal state and geometry/context/lock invalidation. Lock
  presentation bypasses effects; source screenshots/screencasts stay unfiltered.
- Element wrappers must delegate capture, draw, damage/commit and framebuffer-
  effect semantics. Preserve animation scheduling and active-filter scanout
  restrictions. Compilation alone will not detect every missed render hook.

## Build and qualify through existing tools

```sh
python3 waylandshader/build.py --jobs 4 --tests
python3 waylandshader/run-nested.py
```

`--tests` builds the GPU regression; it does not run it. `--support-only` prepares
native dependencies and prints the environment for ordinary Cargo commands.
Keep normal builds locked. Use the commands in the upgrade guide for independent
runtime feature checks and the existing nonvisual test suite.

For a rendering defect, use a deterministic input and compare actual output,
not just active flags or a successful compile. Exercise relevant boundaries:
multipass/temporal frames, mip exposure, parameters, failed-load rollback,
resize, color/bypass and source-capture separation. Run the retained native GL
regression on supported hardware and relevant GL-version paths. A skip is not
success; one GPU or nested window does not certify physical DRM/lock/hotplug,
cross-GPU operation, HDR/VRR, power or latency.

Read the session-debugging skill when launcher/portal behavior differs; do not
edit renderer code to compensate for an incomplete login/service environment.

## Upgrade and package deliberately

Use `waylandshader/check-upstream.py` and the documented merge workflow. Preserve
both fork and upstream work, keep dependency updates separate from architecture
moves when possible, and never downgrade Smithay simply to make old code compile.
A sidecar patch still needs source hooks, rebuilding and semantic validation.

After building an archive:

```sh
python3 waylandshader/verify-package.py /absolute/path/to/niri-waylandshader.pkg.tar.zst
```

This checks the declared package/session structure without installing or running
payloads. It does not verify signatures, ABI behavior, MTREE integrity, hardware
correctness or legal compliance. For a trusted locally built package, separately
exercise its staged binary without build-tree linker overrides and inspect the
private-library RUNPATH. Keep stock installation paths untouched.

Check the exact pinned dependency license and file notices, not only an API
header or badge. Librashader's audited revision offered MPL-2.0 OR GPL-3.0-only;
new revisions still need review. Make matching modified source obtainable and
retain notices before distributing binaries. Naming a `.so` private is not a
license exception.

Record source/dependency revisions, package version, exercised scenarios and
limits. Keep known-good artifacts. Publish only when authorized, without forcing
shared history or silently promoting the development branch to `main`. Never
replace an in-use compositor/prefix; hand native installation to a normal logout
and explicitly chosen next session.

## References

- [Maintenance tools and limits](../../../docs/waylandshader/MAINTENANCE.md)
- [Workspace extraction](../../../docs/waylandshader/REFACTORING.md)
- [Upgrade and release verification](../../../docs/waylandshader/UPGRADING.md#verification-gate)
- [Recorded graphics and lifecycle fixes](../../../docs/waylandshader/HISTORY.md)
- [Project guardrails](../../../AGENTS.md)
