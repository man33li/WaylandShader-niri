# Multi-GPU rendering and GPU-selection plan

Status: phases A–D are implemented and released as 0.3.0; phase E (native-output qualification) is partly done: normal use on both physical monitors works, edge cases are open. See [implementation status](#implementation-status). The research and verification below record the design baseline.

Baseline: niri fork `246a4ce5887cfa8424ff2e39dde6596d41e3d994`, Smithay `22571baa20d34d71092942dbb520c4e3fbbd6263`.

## Decision

Keep **Auto** as the default composition-GPU policy. Run each output's shader on niri's existing primary composition GPU, then let Smithay perform its existing transfer to that output's scanout GPU.

```text
Primary composition GPU
  client textures -> niri scene -> per-output WaylandShader chain
                               -> damage-aware final texture draw
                               -> existing Smithay transfer, if needed
Output GPU                     -> DRM scanout
```

This reuses the current renderer, local EGLImage/fence bridge, and transfer infrastructure. It adds no shader-specific cross-GPU transfer. It is the recommended architecture for minimum integration complexity and transfer work, not a claim that the 680M is faster or more power-efficient for every shader workload.

A global GPU selector remains useful as an advanced **next-login** performance preference. It is not the mechanism that should make HDMI eligible. After cross-GPU integration, either primary GPU can serve outputs on both devices when the actual transfer route supports GPU copies.

### Alternatives

| Approach | Decision |
| --- | --- |
| GPU selector alone | Insufficient. With the current guard, changing the primary GPU exchanges which GPU's outputs are eligible. It does not solve simultaneous multi-GPU output processing. |
| Shader on primary GPU, then existing transfer | Recommended. Preserves niri's primary-context ownership and was verified in both GPU directions. |
| Compose each output on its own GPU | Reject as the default. Requires broader changes to primary-renderer resources, imports, caches, feedback and retirement. |
| Postprocess on the target GPU after transfer | A possible future specialization, not the smallest integration. Requires target-side presentation hooks and shader contexts on multiple device domains. It does not inherently require an extra cross-GPU copy, but is not the path exposed by the current effect element. |
| Remove the same-GPU guard only | Unsafe. Leaves a demonstrated transfer-damage hole and no reliable shader eligibility policy for Smithay's CPU-copy fallback. |

[INFERENCE] Selecting the 6700S may benefit demanding presets at a power cost. Measure real workloads before changing the default or promising a performance gain.

## Source findings

1. `TtyRenderer::as_mut()` and `TtyFrame::as_mut()` access the **render device**, not the target device. The current shader adapter therefore captures and processes the scene before the final cross-GPU transfer.
2. The TTY shader adapter forwards its draw directly to the inner `GlesFrame`. That bypasses `MultiFrame`'s damage accumulation. Ordinary tracked clears/draws can mask this; raw-GLES-only changes need not reach the target GPU.
3. `MultiTexture::from_native_texture` can wrap the ready GLES output texture under its existing render `ContextId`. Drawing this through `MultiFrame::render_texture_from_to` records damage and uses the existing native texture. Its metadata allocation must be cached, not repeated every frame.
4. Smithay attempts actual source allocation/binding and target import. It can fall back to CPU copying after those operations fail. Compatible format/modifier lists alone cannot certify the route.
5. The pinned Smithay implementation keeps the selected transfer state private. The trace field `direct=true` is usable as an experiment oracle, not a production policy API. On same-device frames, `direct=false` does **not** mean CPU fallback.
6. Native `debug { render-drm-device ... }` selects a global renderer at startup. Reloading configuration does not replace that renderer. An unavailable configured GPU can fall back to automatic selection, so saved preference and actual device must be reported separately.
7. Debug settings merge across includes. An absent `render-drm-device` does not clear an earlier explicit value. An empty GUI-owned override cannot by itself guarantee a return to Auto.

Primary evidence:

- [Pinned Smithay renderer and transfer implementation](https://github.com/Smithay/smithay/blob/22571baa20d34d71092942dbb520c4e3fbbd6263/src/backend/renderer/multigpu/mod.rs): render-device forwarding, lines 802–807 and 1043–1061; allocation/import and fallback, 1222–1500; final transfer, 1548–1727; native texture wrapper, 1829–1843; damage-aware frame methods, 2039–2133.
- [TTY backend](https://github.com/man33li/WaylandShader-niri/blob/246a4ce5887cfa8424ff2e39dde6596d41e3d994/src/backend/tty.rs): startup GPU selection and per-output primary-to-target renderer acquisition.
- [Current TTY effect adapter](https://github.com/man33li/WaylandShader-niri/blob/246a4ce5887cfa8424ff2e39dde6596d41e3d994/src/waylandshader/mod.rs) and [runtime](https://github.com/man33li/WaylandShader-niri/blob/246a4ce5887cfa8424ff2e39dde6596d41e3d994/waylandshader/runtime/src/lib.rs): capture, output texture draw and current eligibility guard.
- [Debug-setting merge](https://github.com/man33li/WaylandShader-niri/blob/246a4ce5887cfa8424ff2e39dde6596d41e3d994/niri-config/src/debug.rs#L79-L108) and [optional-value merge semantics](https://github.com/man33li/WaylandShader-niri/blob/246a4ce5887cfa8424ff2e39dde6596d41e3d994/niri-config/src/macros.rs#L21-L29).

## Hardware verification

The experiment used only `/dev/dri/renderD129` (680M, PCI `0000:07:00.0`) and `/dev/dri/renderD128` (6700S, PCI `0000:03:00.0`), offscreen GBM buffers, the pinned Smithay renderer, and a scratch copy of the real WaylandShader runtime linked to the existing native bridge/librashader artifacts. No card node, DRM master, modeset, live-compositor restart or user-settings change was involved.

The only scratch runtime extension exposed the ready output texture. The probe compared the current raw-GLES forwarding with a cached, damage-aware `MultiTexture` draw. A real frame-count-dependent Slang shader processed deterministic scenes. Target-buffer readback was used only as a test oracle, not proposed for production.

Environment: Linux `7.2.6-1-cachyos`, Mesa `26.2.3-arch3.1`, AMD radeonsi. The matrix ran once in the default environment and once with `MESA_GL_VERSION_OVERRIDE=3.3`; Smithay reported GLES 3.2 in both runs.

| Check | Observed result |
| --- | --- |
| Matrix size | 64 cases, 256 frames total across the two environments. |
| Both cross-GPU directions | 680M → 6700S and 6700S → 680M exercised. |
| Actual transfer branch | All 144 cross-GPU frames entered Smithay's imported-dmabuf GPU-copy branch; no CPU-copy fallback observed. |
| Damage negative controls | Raw-GLES scene + raw-GLES effect draw left the target sentinel unchanged in all 16 expected-failure frames. |
| Proposed draw | Cached `MultiTexture` draw updated the target without relying on unrelated tracked scene damage. |
| Pattern/reference equivalence | 24 four-frame comparisons against same-GPU references; maximum RGBA8 readback byte difference was zero. |
| Target formats | `ABGR8888` and `ARGB2101010`. This proves target interoperability, not HDR or 10-bit shader precision. |
| Transforms | Normal, 90° rotation, and `Flipped180`. |
| Resize | 64×48 → 80×40 within each four-frame case. |
| Wrapper reuse | Two wrapper constructions per four-frame candidate case: one for each output-texture lifetime/size, not one per frame. |

The negative controls are part of the successful verification: they demonstrate why guard removal alone is not the fix. The 256 frames must not be described as 256 successful presentation frames.

The research evidence was kept locally, outside version control, in the development checkout's ignored `build/gpu-routing-evidence-20260923-zbhhx8tn/`: `summary.json`, the default and GL 3.3 override matrices, per-case transfer evidence (`routes.json`), raw traces and `probe-source.tar.gz` (the probe inputs; its `settings.json` names a local preset path).

These local artifacts are not release assets or tracked dependencies. Small offscreen buffers do not certify physical HDMI scanout, sustained performance, latency, power, simultaneous temporal histories, capture/lock behavior, hotplug or failure recovery.

## Implementation plan

### A. Correct the presentation draw while retaining the guard

Files: `waylandshader/runtime/src/lib.rs`, `src/waylandshader/mod.rs` and existing per-output graphics state.

- Expose a narrow ready-output-texture view with a texture lifetime/generation; keep native bridge internals private.
- Keep capture and shader execution on the render GLES context.
- Cache `MultiTexture::from_native_texture` in persistent per-output state, keyed by render `ContextId` and texture generation. Do not put a newly allocated cache on each transient render element or rely on a numeric GL texture name alone.
- Perform the actual final TTY draw through `MultiFrame::render_texture_from_to`. Preserve the existing source size, destination geometry and inverse output transform. Do not add a fake transparent draw, CPU readback or `glFinish` merely to repair damage.
- Drop the wrapper before its imported GLES/native resources during texture replacement, failure, output/context retirement and teardown. Keep the direct GLES/winit presentation path valid.

Acceptance: retain the isolated damage counterexample as a focused regression scenario; the candidate must update the complete target with no unrelated tracked scene draw. Same-GPU and nested-backend rendering must remain correct.

### B. Make the actual transfer route observable

Dependency boundary: the workspace's single Smithay revision, including `smithay-drm-extras` consistency.

- Add a small public, read-only observation of the actual route: same-device, imported-dmabuf GPU copy, or CPU copy; unknown must remain distinguishable before allocation/import.
- Make the route observable before shader capture, with enough backing-allocation/context identity to invalidate cached eligibility correctly. A post-finish log message is too late to be the policy boundary.
- Prefer an upstreamable change to the pinned dependency over copying multigpu internals into niri. Do not infer successful import from format intersection or parse tracing output in production.
- Leave Smithay's normal unfiltered-desktop fallback intact. The shader integration must bypass processing on unknown/CPU routes and report the reason.

Acceptance: exercise real or injected allocation/import failure, actual CPU fallback, and recovery after resource invalidation. Confirm that shader capture is skipped on unsupported routes and the unfiltered scene remains usable. This rejection policy was **not** implemented or exercised by the research probe.

### C. Replace node equality with verified presentation eligibility

Files: `src/backend/tty.rs`, `waylandshader/runtime/src/lib.rs`, the TTY adapter and status/control plumbing.

- Remove the physical-node-equality restriction only after A and B are complete. Migrate every caller of the eligibility contract, including same-device/nested callers; do not retain an obsolete alias.
- Base support on the actual route and existing renderer/bridge capabilities. Establish the route with a bounded composition frame on enable/invalidation where needed; do not create a permanent probe/redraw loop for unsupported outputs.
- Invalidate eligibility and texture caches on backing-allocation, size/format, context/device and output changes. Retry on meaningful invalidation, not every frame.
- Preserve full-output shader animation damage and disable direct scanout/overlay bypass while filtering is active.
- Preserve capture bypass, lock/history resets, independent per-output histories, transactional preset rollback and shader-resource retirement before backend destruction.
- Keep transport eligibility distinct from preset compilation failure: an unsupported output must not indefinitely block transactions or be reported as successfully processing.

Acceptance: both physical displays process correctly together with the primary GPU left on Auto; then qualify the reverse primary-GPU choice. Verify shader-only animation, real capture and lifecycle/failure paths listed below.

### D. Expose GPU status and the advanced preference accurately

Integration: native startup GPU policy, the existing control service and Qt settings surface. Do not introduce a second hidden preference authority.

- Show the active composition GPU and each output's target GPU, processing state and bypass reason.
- Offer Auto and discovered eligible render devices as a **global, next-login** preference. Use stable device identity/by-path names where available; do not hard-code `renderD128`/`renderD129` as persistent identities.
- Distinguish configured preference, actual active device, pending change and startup fallback. Saving a preference must not claim that the active renderer switched.
- Report the active config path from the compositor rather than guessing it; account for a custom `--config` path and runtime config-file selection.
- Save only through an explicitly adopted, validated native-KDL configuration path/owned include. Preserve unrelated debug settings, includes, symlinks and concurrent user edits; use atomic writes, conflict detection and rollback.
- Before offering Auto as effective, verify that no earlier/unmanaged include still sets `render-drm-device`. An empty managed fragment does not clear an earlier override. Externally managed/read-only/conflicting configurations need an honest read-only/manual-config state, not a false successful save.
- Keep disk I/O and validation off the compositor render thread. Do not restart the compositor automatically.

Acceptance: Auto/explicit choice, active-versus-pending display, missing-device fallback, custom config path, include precedence, Auto restoration, conflicting edits, read-only files, symlink preservation and failed-save rollback. The selector is not required to make the automatic cross-GPU rendering path work.

### E. Native qualification and release gate

Before removing the limitation from release documentation:

- Both physical outputs, their real modes/formats, rotation and fractional scale; correct animated full-output effects with otherwise static content.
- Representative presets, temporal/history shaders and independent per-output state; resize, transform, disable/enable, lock/unlock, hotplug and device retirement.
- Source screenshots and screencasts remain unfiltered; lock/capture exclusions remain intact.
- Invalid preset, failed bridge/import, CPU-only transfer and target-copy errors preserve a usable scene and correct rollback/status. No stale processed buffer or redraw storm.
- Repeated lifecycle operations show stable resource usage and correct teardown ordering.
- Measure frame time, missed deadlines, CPU usage and power under representative resolutions/refresh rates with shaders off/on and each primary GPU. Compare against the existing transfer baseline; the readback-heavy probe is not a benchmark.
- Run the affected build/tests once integration is complete, update existing documentation/changelog only after native evidence supports the claimed behavior, and retain the focused damage regression rather than a permanent copy of this research workspace.

A and B can be developed independently while the production guard remains. C depends on both. GPU status/configuration work is a separate control-plane concern; it must not become a prerequisite for the rendering fix.

## Implementation status

| Phase | Implementation |
| --- | --- |
| A | `src/waylandshader/mod.rs` draws the ready output texture through `MultiFrame::render_texture_from_to`. The `MultiTexture` wrapper lives in a host slot of the runtime's per-output state (`ShaderElement::with_output`), keyed by render `ContextId`; the runtime clears it with the textures it wraps, before native storage. |
| B | Implemented without patching Smithay, which the plan preferred but which forced every Cargo command through a build-time Smithay checkout. `gpu_copy()` in `src/waylandshader/mod.rs` repeats, with public APIs, the checks of Smithay's `create_shared_dma_framebuffer` (cross-device capability, shared explicit modifiers, allocation, bind and target import). The TTY backend runs it when an output's mode, format or GPU changes and caches the result per output. It is a prediction: a failure Smithay hits only later (for example a transient allocation failure) is not observed. |
| C | `Manager::prepare` takes the output's `Transfer` instead of a same-GPU flag. CPU-copy outputs get no effect element and no animation redraws, and report a reason; nested sessions pass `None`. Compilation no longer depends on eligibility. |
| D | `src/waylandshader/gpu.rs` publishes the render GPU, each output's GPU, the startup request/fallback, the configured next-login GPU and the preference state. `setRenderDevice` writes only an adopted `waylandshader-gpu.kdl` next to the loaded configuration, validates the resulting configuration and rolls back on conflicts. The Qt controller shows per-output routes and a Render GPU section. |

Verified on the development machine (Radeon 680M + RX 6700S, Mesa 26.2.3):

- `waylandshader::tests::cross_gpu_presentation` (ignored; two render nodes): both directions, `ABGR8888`/`ARGB2101010`, three transforms and resize match a tracked-damage reference per pixel. With the old inner-frame draw restored it fails immediately (the target kept its sentinel), so it guards the damage path. It requires `gpu_copy()` to predict a GPU copy in every case, fails if Smithay warns about or falls back from such a transfer, and checks that CPU-copy outputs stay unfiltered and idle.
- `waylandshader::gpu::tests`: adoption, validation, later/earlier override precedence with rollback, hand-edited, read-only and symlinked managed files.
- A throwaway private-bus run of the control service with both real GPUs: device names and by-path identities, Qt client status parsing (`waylandshader-nirictl status`) and `setRenderDevice` saves, rejection and automatic restoration.
- Nested preview (winit) on a private bus: after enabling and loading a preset, the nested output reported the preset active with the new `prepare` API and no `gpu` section. The controller, fed a mock native-session status, showed the Render GPU section (automatic, pending, fallback and not-adopted states) and the per-output "Unfiltered" reason; screenshots were inspected. Clicking Save was not driven (no input injection tool); its D-Bus call was exercised above.
- The workspace test suite (200 niri library tests plus the other crates), the no-D-Bus build and the independent runtime checks passed.
- Native session, 0.3.0 package: the user reported that effects work as intended with eDP-2 on the 680M and HDMI-A-1 on the RX 6700S.

Not verified: lock/unlock, suspend/resume, hotplug and device retirement, capture on native outputs, saving the render GPU and logging in with it, a real Smithay CPU-copy fallback (not reproducible on this hardware without driver fault injection), and performance or power. These remain phase E work; record results in [HISTORY.md](HISTORY.md) as longer use produces them.
