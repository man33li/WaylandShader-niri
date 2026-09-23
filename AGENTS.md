# Agent guidance for WaylandShader-niri

This repository is a source-built niri fork plus a source-linked shader runtime.
The root Cargo workspace is the compositor source; do not fetch a second niri
copy, restore the old pinned niri patch, or present the runtime as a stock-niri
plugin. Keep changes narrowly scoped to the requested behavior.

## Read the relevant workflow first

- Desktop/session/Flatpak/portal/logout problems:
  [.agents/skills/wayland-session-debugging/SKILL.md](.agents/skills/wayland-session-debugging/SKILL.md).
- Renderer/FFI/lifecycle/upstream/package changes:
  [.agents/skills/wayland-shader-integration/SKILL.md](.agents/skills/wayland-shader-integration/SKILL.md).
- Commands and evidence boundaries:
  [maintenance toolkit](docs/waylandshader/MAINTENANCE.md).
- Smithay updates (normally through upstream niri merges):
  [following Smithay upstream](docs/waylandshader/SMITHAY.md).
- Established architecture, choices and history:
  [refactoring](docs/waylandshader/REFACTORING.md),
  [design decisions](docs/waylandshader/DECISIONS.md),
  [session lifecycle](docs/waylandshader/README.md#managed-session-startup-and-shutdown),
  [upgrades](docs/waylandshader/UPGRADING.md).

## Protect the running desktop

- Default to read-only evidence collection. Do not reboot, suspend, reset GPUs,
  change boot parameters, restart the display manager/compositor/portals, or
  replace an in-use installation to investigate a report.
- Accept user-reported failures; establish causes and verify the fix rather than
  repeatedly asking the user to reproduce what is already known.
- Use the existing nested preview for rendering checks. Full desktop/portal
  lifecycle tests additionally need private HOME, runtime, config, cache, state,
  data, D-Bus and activation environment; the rendering helper alone is not that
  complete sandbox. A private bus with host activation data can launch helpers
  outside the intended test environment.
- Before sending input or IPC, verify the target executable, PID and private
  socket. Never inject into the live user's seat as a shortcut.
- Tests involving systemd must use unique owned unit/target names, not the live
  graphical target. Record readiness, stop and remove only owned resources, and
  confirm that the live compositor/shell were not replaced.
- Build into a fresh/versioned prefix. Keep known-good packages. System-wide
  installation and native-session qualification require an explicit safe handoff.

## Preserve the desktop integration contract

- The login entry uses `niri-waylandshader-session`, not the raw compositor's
  `--session` flag. Derive launcher/units from `resources/`; do not maintain a
  second independent implementation of upstream session management.
- Preserve login-shell initialization, environment import before startup,
  compositor READY ordering, graphical/XDG autostart targets and logout cleanup.
- Keep stock niri's files untouched and retain `DesktopNames=niri` and the niri
  desktop identity. Separate unit names are not simultaneous-session isolation.
- Portals are D-Bus activated; audio/keyring services have their own lifetimes.
  Do not globally enable the compositor or add duplicate Noctalia, polkit,
  PipeWire, keyring, portal or Xwayland launchers to hide a lifecycle defect.
- Inspect the actual shell version and active configuration paths. Native
  Noctalia 5 is not the old Quickshell shell and does not use its legacy JSON
  settings as the native configuration.

## Preserve rendering and ownership invariants

- Keep niri policy/adaptation in `src/waylandshader/` and shader implementation
  in `waylandshader/runtime/`. Share the workspace Smithay revision and preserve
  the root D-Bus feature forwarding. Trace every affected caller before editing.
- GLES and desktop GL contexts are unshared. Preserve EGLImage storage ownership,
  EGL binding restoration, GPU fences/server waits and the private library ABI.
  Do not replace these with cross-API context sharing, CPU readback or a capture
  portal loop. Do not add per-frame `glFinish` as an unexplained workaround.
- Preserve transactional compilation/rollback, success-only recent history,
  per-output temporal state, geometry/context/lock resets and unsupported-GPU
  error/bypass behavior. Do not special-case a particular user preset.
- Final presentation is filtered; source captures are not. Preserve framebuffer
  capture, damage/commit semantics, animation scheduling and active-filter
  scanout restrictions through every element wrapper.
- TTY presentation draws through `MultiFrame`, never its inner GLES frame: only
  tracked damage reaches a monitor on another GPU. Outputs whose frames would
  reach their GPU by CPU copy stay bypassed; `waylandshader::gpu_copy` repeats
  Smithay's transfer checks and must follow its multi-GPU code on Smithay bumps.
- Retire workers/native shader resources before backend/EGL display destruction.
  Trace actual shared ownership and EGLImage sibling semantics, not field names
  alone. An INFO SIGTERM line is not proof of a crash or completed destruction.
- Do not expose uninitialized mip levels. Keep the retained native regressions
  for temporal history, mip exposure, orientation, color and GL-version paths.

## Evidence, releases and scope

- Read NUL-delimited `/proc/PID/environ` with a strict allowlist and identity
  checks. Manager environment is not evidence of an existing child's environment;
  `/proc` normally describes its initial exec environment, not every later update.
- Missing/inaccessible logs, processes or services are unknown, not healthy.
  Separate current process snapshots from historical boot logs. Review diagnostic
  reports before sharing; opt-in journal messages can contain personal data.
- Classify compositor, portal, display-manager and kernel messages separately.
  Correlate stock/fork/greeter timestamps and verify status codes against the
  relevant source. Do not attribute AMDGPU errors to shaders merely because they
  appear after a compositor exits.
- Run the affected program/path and keep tests only for plausible behavioral
  regressions. Parser/privacy/unsafe-archive cases deserve tests; wording,
  forwarding and mock echoes do not. Nested success is not physical DRM, lock,
  hotplug, reboot, HDR, VRR, latency or power certification.
- Inspect actual package contents, managed-session assets, stock path isolation,
  notices and corresponding source. A package checker or license-file presence
  does not establish authenticity, ABI correctness or legal compliance. Verify
  the exact pinned dependency license, not just an upstream badge or API header.
- Keep source history merge-based, pinned dependencies deliberate and user work
  intact. Use the existing candidate checker; no force-push, automatic promotion,
  live installation or unsolicited global agent configuration.
