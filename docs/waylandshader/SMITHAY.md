# Following Smithay upstream

[Upgrades and rollback](UPGRADING.md) · [Design decisions](DECISIONS.md) ·
[History](HISTORY.md) · [Maintenance toolkit](MAINTENANCE.md)

[Smithay](https://github.com/Smithay/smithay) is the compositor library niri is
built on. WaylandShader-niri does not patch it and does not pin it separately:
the root `Cargo.toml` pins one Git revision, shared by niri, the
`waylandshader-runtime` crate and `smithay-drm-extras`:

```toml
[workspace.dependencies.smithay]
git = "https://github.com/Smithay/smithay.git"
rev = "79bbed5e1199090d787115614847a79c76607181"

[workspace.dependencies.smithay-drm-extras]
git = "https://github.com/Smithay/smithay.git"
rev = "79bbed5e1199090d787115614847a79c76607181"
```

niri pins Git revisions of Smithay's main branch, not releases. This guide
covers how a newer Smithay reaches this fork, what to review for WaylandShader,
and how to verify and ship it.

## Choose the route

1. **Normal: merge upstream niri.** niri updates its pin together with any code
   changes the new Smithay needs. Merging official niri brings both, and keeps
   the fork on a combination upstream uses. Follow
   [UPGRADING.md](UPGRADING.md); this guide adds the Smithay-specific review in
   [Review what changed](#review-what-changed) and
   [Verify](#verify).
2. **Exception: out-of-band Smithay update.** Use this only for a Smithay fix
   you need before niri adopts it (for example, a crash or power regression you
   are affected by), and only if niri's code builds unchanged against it. Follow
   [Out-of-band update](#out-of-band-update).

Never downgrade Smithay to make old code compile. Keep both `rev` lines equal.
Do not point Cargo at a branch name, a local path or a patched checkout: the
build must reproduce from the committed `Cargo.lock`.

## Check what is available

From the repository root, with the `upstream` remote from
[UPGRADING.md](UPGRADING.md#initial-setup-and-publication):

```sh
pin() {
    git show "$1:Cargo.toml" |
        sed -n '/^\[workspace.dependencies.smithay\]/,/^\[/s/^rev = "\(.*\)"$/\1/p'
}
git fetch upstream main
current=$(pin HEAD)
niri=$(pin upstream/main)
echo "fork: $current"
echo "niri: $niri"

work=$(mktemp -d)
git clone --quiet --filter=blob:none https://github.com/Smithay/smithay.git "$work/smithay"
latest=$(git -C "$work/smithay" rev-parse origin/HEAD)
git -C "$work/smithay" log --oneline "$current..$niri"
git -C "$work/smithay" log --oneline "$niri..$latest"
```

- If `niri` differs from `current`, the normal route applies: merge upstream niri.
- Commits in `niri..latest` are not yet in niri. Wait for niri unless one of them
  fixes a problem you actually have.

## Review what changed

Set `new` to the revision you are moving to (`$niri` for a merge), then list the
changes in the areas WaylandShader depends on:

```sh
new=$niri
git -C "$work/smithay" diff --stat "$current" "$new" -- \
    src/backend/renderer src/backend/egl src/backend/allocator \
    src/backend/drm/compositor src/output.rs src/wayland/session_lock
```

| Smithay area | Why WaylandShader depends on it |
| --- | --- |
| `backend/renderer/multigpu/` | `gpu_copy()` repeats `create_shared_dma_framebuffer`; the TTY adapter draws through `MultiFrame` with `MultiTexture::from_native_texture`. |
| `backend/renderer/gles/` | The runtime captures and presents inside `GlesFrame`, imports EGLImages as `GlesTexture`s, and relies on frame finish to release dead dma-buf imports. |
| `backend/renderer/element/`, `damage/` | Framebuffer-effect capture/draw order, the `render_elements!` macro and damage semantics of the effect element. |
| `backend/egl/` | EGL display/context lifetime and EGLImage import used by the native bridge. |
| `backend/allocator/` | `gpu_copy()` allocates its trial buffer with the GBM allocator and format sets. |
| `backend/drm/compositor` | Frame flags: active filtering disables direct scanout and overlay planes. |
| `output.rs` | Mode, transform and scale form the per-output geometry key. |
| `wayland/session_lock` | Lock presentation bypasses effects. |

Then compare Smithay's CPU-copy fallback with `gpu_copy()` in
`src/waylandshader/mod.rs`:

```sh
git -C "$work/smithay" diff "$current" "$new" -- src/backend/renderer/multigpu/mod.rs
```

Read `create_shared_dma_framebuffer` and `MultiRenderer::render` in the new
revision. If the conditions for a shared transfer buffer changed (capability
checks, the format/modifier selection, allocation, binding or the target
import), update `gpu_copy()` in the same change so it makes the same decision.
If Smithay now offers a public way to learn the path it chose, use it and
delete `gpu_copy()`.

Read the commit messages for behavior changes too: renderer cleanup, context
handling, damage tracking or scanout changes can affect the effect without
changing any API WaylandShader calls. Delete the clone afterwards with
`rm -rf "$work"`.

## Out-of-band update

Only after deciding the exception applies:

1. Prepare the recoverable baseline in
   [UPGRADING.md](UPGRADING.md#prepare-a-recoverable-baseline) (tag, bundle,
   installed package, settings).
2. Work on a named branch, from clean committed source:

   ```sh
   git switch -c "upgrade/smithay-$(printf %.8s "$new")"
   sed -i "s/^rev = \"$current\"$/rev = \"$new\"/" Cargo.toml
   grep -c "rev = \"$new\"" Cargo.toml   # must print 2
   cargo update -p smithay --precise "$new"
   git diff --stat
   ```

   `cargo update -p smithay` moves `smithay-drm-extras` too, because both come
   from the same Git source. Inspect the `Cargo.lock` diff: only Smithay and
   dependencies it added or dropped may change. Do not run an unrestricted
   `cargo update`.
3. Review and verify as below, then commit with the reason, for example
   `Update Smithay to <short sha> for <fix>`.

At the next upstream niri merge, `Cargo.toml` may conflict. Keep niri's pin when
it is the same or newer. If niri's pin is older than yours, keep yours only if
niri's merged code still builds and passes the gate; record that decision in
[HISTORY.md](HISTORY.md).

## Verify

Run the complete [verification gate](UPGRADING.md#verification-gate) with a
fresh `python3 waylandshader/build.py --jobs 4 --tests`. For Smithay changes the
following parts are essential, not optional:

- `build/niri-support/niri_bridge_test` on each GPU (`DRI_PRIME=1` selects the
  other one on hybrid Mesa systems), with and without
  `MESA_GL_VERSION_OVERRIDE=3.3`. Exit 77 is a skip, not a pass.
- On a two-GPU machine, the retained cross-GPU test:

  ```sh
  WAYLANDSHADER_TEST_RENDER_NODES='/dev/dri/renderD129 /dev/dri/renderD128' \
    cargo test --locked --lib waylandshader::tests::cross_gpu_presentation -- --ignored
  ```

  List your own two render nodes. It presents the shader through Smithay's
  transfer in both directions and fails when presentation damage stops reaching
  the target GPU, when `gpu_copy()` rejects a transfer, or when Smithay warns
  about or falls back from a transfer `gpu_copy()` predicted as a GPU copy.
- `python3 waylandshader/run-nested.py`: load a preset and confirm the effect,
  bypass, rollback and resize still work.

On the native session after installing the package, check both monitors with
`waylandshader-nirictl status`: each output's `transfer` should be `same-gpu` or
`gpu-copy`, `shaderActive` true where enabled, and `bypass` empty. Also check
lock/unlock, suspend/resume and hotplug when the renderer or DRM code changed.

## Record and ship

- Add a [HISTORY.md](HISTORY.md) entry: old and new Smithay revisions, the
  reason, the route, anything adjusted (such as `gpu_copy()`), verification and
  remaining limits.
- For a new package: a Smithay change with no WaylandShader feature change
  increments `pkgrel` in `waylandshader/PKGBUILD`; a new extension version
  follows the rules in [UPGRADING.md](UPGRADING.md#merge-on-an-upgrade-branch).
- Build, install and roll back as in
  [UPGRADING.md](UPGRADING.md#install-and-roll-back-without-replacing-a-live-compositor).
  Keep the previous package archive until the new one is verified natively.

## Example: 0.3.0

Upstream niri `5f4469b6` moved Smithay from `22571baa` to `79bbed5e` with
changes to `Cargo.toml` and `Cargo.lock` only. The six new commits touched
the GLES renderer (context activation during cleanup, a GLES 2.0 texture binding
fix), frame callbacks, keyboard hashing, layer-shell and session-lock commit
hooks. The multi-GPU module and GBM allocator were byte-identical, so
`gpu_copy()` was unchanged; frame finish still releases dead dma-buf imports.
The route was an upstream niri merge. See [DECISIONS.md](DECISIONS.md#7-smithay-follows-niris-pin-through-upstream-niri-merges).
