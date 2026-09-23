# Design decisions

[Build and controls](README.md) · [Multi-GPU plan](GPU-PLAN.md) ·
[Following Smithay](SMITHAY.md) · [Upgrades](UPGRADING.md) · [History](HISTORY.md)

This records the choices behind extension **0.3.0** (package `26.04.ws0.3.0-1`):
shader effects on monitors driven by another GPU, the render GPU preference, how
Smithay is used and updated, and how the release is versioned and promoted. Each
entry states the decision, why it was taken, what was rejected, and what it
costs. The research and hardware evidence behind the first decisions is in the
[multi-GPU plan](GPU-PLAN.md).

## 1. Effects run on niri's render GPU, before Smithay's transfer

niri composes every output on one render GPU chosen at login. WaylandShader
processes each output there, then Smithay's existing multi-GPU renderer copies
the finished frame to the GPU that scans the monitor out.

- **Why:** the shader runtime, its EGLImage exchange and niri's texture caches
  all live on the render GPU. This adds no shader-specific cross-GPU copy and
  no second shader context per device.
- **Rejected:**
  - A GPU selector alone: it only changed which monitors could be filtered.
  - Composing each output on its own GPU: it would rework niri's primary
    renderer caches, imports, feedback and teardown.
  - Post-processing on the monitor's GPU: it needs target-side hooks and a
    shader context per device.
  - Removing the same-GPU guard alone: see decision 2.
- **Cost:** a monitor on the other GPU still costs one GPU copy per frame, as it
  did unfiltered. Performance and power were not measured.

## 2. The presentation draw goes through `MultiFrame`

The TTY adapter draws the shader output with `MultiFrame::render_texture_from_to`,
using the output texture wrapped once as a `MultiTexture`.

- **Why:** drawing on the inner GLES frame bypassed Smithay's damage tracking,
  so a monitor on another GPU could keep an old frame. The research probe
  reproduced it: the target kept its test colour.
- **How:** the wrapper lives in a host slot of the runtime's per-output state
  (`ShaderElement::with_output`), keyed by the render `ContextId`. The runtime
  empties the slot whenever it replaces or retires the texture, before the
  native storage goes away. Nothing is allocated per frame.
- **Test kept:** `waylandshader::tests::cross_gpu_presentation` (ignored; needs
  two GPUs) fails immediately if the draw returns to the inner frame.

## 3. Monitors reached only by CPU copies stay unfiltered

When Smithay cannot share a buffer between the GPUs, it reads every frame back
into memory and uploads it again. Such monitors keep the plain desktop and report
"Only CPU copies reach this monitor's GPU".

- **Why:** an animated shader would force that readback on every frame. The
  unfiltered desktop is correct and cheap.
- **Cost:** those monitors cannot use effects. Smithay's CPU-copy fallback could
  not be produced on the development hardware, so this path is only tested by
  simulating the report.

## 4. Smithay is used unmodified; the transfer path is predicted

`gpu_copy()` in `src/waylandshader/mod.rs` repeats, with public APIs, the checks
Smithay's `create_shared_dma_framebuffer` makes before falling back to CPU copies:

1. Both GPUs allow cross-device sharing.
2. They have a shared explicit modifier for the output's format.
3. A buffer of the output's size can be allocated and bound on the render GPU
   and imported on the monitor's GPU.

The TTY backend runs it when an output's mode, format or GPU changes and caches
the answer per output.

- **Why:** the first implementation patched Smithay to expose the path it chose.
  That made every Cargo command depend on a build-time Smithay checkout plus a
  maintained patch. The prediction needs neither.
- **Rejected:**
  - The Smithay patch: implemented, then removed.
  - Reading Smithay's trace spans: log output is not an API.
  - Always filtering: CPU readback of every animated frame.
- **Cost:** `gpu_copy()` must follow Smithay's fallback code. The
  [Smithay guide](SMITHAY.md) makes that comparison part of every update. The
  two-GPU test fails if Smithay warns about or falls back from a transfer that
  `gpu_copy()` predicted as a GPU copy. A failure Smithay hits only later (for
  example, a one-off allocation failure) falls back to CPU copies with the effect
  still on: the image stays correct, just slower.
- **Exit:** if Smithay gains a public accessor for the chosen path, use it and
  delete `gpu_copy()`.

## 5. Automatic GPU selection stays the default

The controller shows the render GPU in use, the GPU behind each monitor, the
startup request and whether it fell back. A different render GPU can be saved
as a **next-login** preference.

- **Why:** niri reads `debug { render-drm-device }` once, at startup; there is no
  live switch. With decisions 1–4, either GPU can serve monitors on both, so the
  choice is only a performance/power trade-off, not a requirement.
- **Rejected:** switching live (niri cannot) or restarting the compositor from
  the controller (would end the user's session).
- **Cost:** a saved choice is pending until the next login; the status reports
  it separately from the GPU in use, and reports a missing saved GPU as a
  fallback.

## 6. The preference is written only to an explicitly adopted include

The controller writes `waylandshader-gpu.kdl` next to the configuration niri
loaded, and only after the user added
`include optional=true "waylandshader-gpu.kdl"` to that configuration.

- **Why:** the user's KDL (comments, includes, symlinks) is never rewritten. The
  managed file holds one stable `/dev/dri/by-path` device.
- **Safety:**
  - Each save reloads the configuration to confirm that the saved GPU is the one
    niri will use.
  - If another file overrides it, the previous managed file is restored and the
    reason reported.
  - An empty managed file cannot clear an earlier `render-drm-device`: niri merges
    `debug` settings across includes and an absent value resets nothing.
  - Hand-edited, read-only and symlinked managed files are refused.
- **Rejected:** editing `config.kdl` directly; keeping the preference in
  WaylandShader's JSON (a second authority that niri would ignore).

## 7. Smithay follows niri's pin, through upstream niri merges

0.3.0 merges official niri `5f4469b6` (2026-09-22). Its only Smithay-related
change moved the pin from `22571baa` to `79bbed5e`, with no niri code changes.

- **Why:**
  - niri tests its own code against its pin, and merging keeps the fork on a
    combination upstream uses.
  - The new Smithay only makes a GPU's context current when there is cleanup to
    do, because activating an idle GPU's context wakes it from runtime suspend
    (niri calls this the "dGPU context regression" fix). This is relevant to
    hybrid laptops.
  - It also fixes a screenshot regression and stops frame callbacks to unmapped
    surfaces.
- **Rejected:** bumping Smithay alone (an untested niri/Smithay pairing and a
  deviation from the merge-based workflow); tracking Smithay's main branch
  continuously (main also introduces regressions).
- **Merge resolutions:**
  - The fork keeps its deletion of the inherited `flake.nix` and RPM/DEB
    packaging metadata.
  - Upstream's new `png` dev-profile optimisation is kept.
  - Upstream's session launcher change (`exec "$SHELL" -l -c …`) flows into the
    generated `niri-waylandshader-session` unchanged.
- **Check:** Smithay's multi-GPU module and GBM allocator are identical between
  the two revisions, so `gpu_copy()` needed no change.
- **Procedure:** see the [Smithay guide](SMITHAY.md), including when an
  out-of-band Smithay update is justified.

## 8. The release is extension 0.3.0

`pkgver` becomes `26.04.ws0.3.0` with `pkgrel=1`; `waylandshader/CMakeLists.txt`
and the runtime crate move to `0.3.0`.

- **Why:** the control interface gained `setRenderDevice` and new status fields,
  and the runtime API changed (`Manager::prepare` takes the output's transfer).
  The upgrade guide asks for a new extension version, not another `pkgrel`, for
  such changes.
- **Cost:** none for users; settings JSON is unchanged, so rolling back to
  `0.2.2` keeps settings.

## 9. Installation and promotion wait for native verification

The package is built locally and installed by the user from outside the running
session. The branch is pushed and `main` fast-forwarded only after the user has
verified both monitors on the physical desktop.

- **Why:** nested sessions and render-node tests do not exercise physical
  HDMI/eDP scanout, lock, hotplug or power. The running compositor is never
  replaced under the user.
- **How:** local `main` is an ancestor of `refactor/waylandshader-workspace`, so
  promotion is a fast-forward; no history is rewritten.
