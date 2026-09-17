---
name: wayland-session-debugging
description: Diagnose missing Flatpak launcher entries, broken desktop portals or file choosers, session environment drift, missing compositor logs, and logout or reboot messages in niri/WaylandShader desktops. Use when an application works under stock niri but fails in the fork, or when service lifecycle and kernel errors need to be separated.
compatibility: Requires this source checkout and a Linux desktop user context; Python and optional systemd/journalctl/Flatpak tools. Read-only collection needs no root or GUI restart.
---

# Wayland session debugging

Start with the smallest evidence that distinguishes a compositor bug from a
session, application, portal or kernel problem. Accept the reported symptom;
do not restart the user's desktop to confirm it.

## Collect before changing anything

Run from this repository's root:

```sh
python3 waylandshader/diagnose-session.py
python3 waylandshader/diagnose-session.py --journal --boot 0 --lines 80
```

Use `--pid` for a specific current, same-user process. Journal capture is opt-in;
review it before sharing. The report is evidence, not a pass/fail hardware test.
An unavailable source must remain unknown. For a previous shutdown, collect
`--journal --boot -1` separately: process/unit snapshots still describe now,
not that historical boot.

## Trace the full launch chain

1. Identify the actual compositor/shell executable, version, PID, cgroup and
   stdout/stderr destination. A detached PPID alone does not identify its original
   launcher. A TTY-only log may never have entered the journal.
2. Read the selected display-manager `.desktop` entry and session wrapper. The
   raw compositor `--session` option is not equivalent to stock `niri-session`.
3. Compare only relevant process and manager environment fields: XDG data roots,
   desktop/session identity, display/IPC sockets and active config roots. Decode
   `/proc/PID/environ` as NUL-delimited data; do not dump secrets or infer absence
   from a text search that cannot read it. Its initial environment may differ
   from later in-process `setenv` state.
4. For missing Flatpaks, check actual export directories and desktop visibility
   metadata against the shell's own search roots. Reuse the installed login-shell
   integration; do not hardcode paths in the launcher to disguise skipped setup.
5. For a chooser, follow the frontend, configured backend, D-Bus activation and
   unit dependencies. `FileChooser` appearing in introspection does not prove
   the selected backend can start. Check its graphical-session prerequisite.
6. Inspect active configuration for the installed shell version. Native Noctalia
   5 uses TOML/state-sidecar configuration, not the legacy Quickshell JSON.

## Restore ownership, not symptoms

For this fork the intended chain is:

```text
login entry -> login-shell session wrapper -> environment import
  -> notify compositor service -> display/D-Bus readiness
  -> graphical session and XDG autostart targets
```

Keep the separately named stock/fork services and `niri` desktop identity.
Reuse the checked-out upstream session resources. Do not remove a portal's
`Requisite`, force another backend, enable every service, or duplicate shell,
keyring, audio, polkit or Xwayland autostarts merely to silence an error.

On ordinary compositor logout, portal clients can report a lost Wayland
connection before target cleanup; stock niri can do this too. Ordered target
shutdown reverses dependency ordering. Noctalia reboot/poweroff uses the normal
systemctl/logind/PID 1 path, not a shader-specific GPU shutdown procedure.

## Separate kernel and userspace evidence

- `quitting due to receiving signal SIGTERM` is normal graceful-stop logging.
- Compare errors before fork startup, under stock/greeter sessions, and around
  exit. Match boot/kernel/compositor revisions; do not reuse an old PID or device
  mapping as a current fact.
- Determine what a numeric code means from the matching driver source. The
  recorded AMD DMUB status 2 was a display-microcontroller queue-full status,
  not a shader compiler error or errno 2. Do not generalize that diagnosis to
  every future driver/version.
- Do not apply speculative GPU reset, PSR, power-management or boot-parameter
  workarounds. A temporal correlation is not a demonstrated shader defect.

## Verify safely

Use the existing isolated preview for rendering. For lifecycle checks, use
unique private targets, runtime paths, D-Bus and activation environment; confirm
READY, actual UI behavior and cleanup. Never substitute the live graphical
session target. If input is needed, verify its private socket first.

Exercise the real launcher/chooser when practical. State whether the exact
sandboxed application/transient-parent flow was tested. A nested non-session
compositor can lack Mutter's session channel and is not native logout/reboot
certification. Preserve logs/screenshots privately and remove owned helpers.

Deliver the causal chain, exact evidence, changed source, verification scope,
remaining unknowns and safe next-login/rollback instructions. Do not claim the
running desktop is repaired when only a new package was built.

## References

- [Maintenance tools](../../../docs/waylandshader/MAINTENANCE.md)
- [Managed startup and shutdown](../../../docs/waylandshader/README.md#managed-session-startup-and-shutdown)
- [Recorded session regression](../../../docs/waylandshader/HISTORY.md#managed-desktop-session-parity-022-2)
- [Project guardrails](../../../AGENTS.md)
