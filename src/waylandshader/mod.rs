//! Niri-specific monitor identity, redraw registration, and renderer adaptation.

use smithay::backend::renderer::element::{render_elements, RenderElement};
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::output::Output;
use smithay::utils::user_data::UserDataMap;
use smithay::utils::{Buffer, Physical, Rectangle};

use crate::backend::tty::{TtyFrame, TtyRenderer, TtyRendererError};
use crate::niri::State;
use crate::render_helpers::renderer::AsGlesFrame as _;

pub fn output_profile(output: &Output) -> String {
    // Serial-bearing monitors retain settings across connector changes.
    // Indistinguishable/no-serial panels use niri's connector selector.
    output
        .user_data()
        .get::<niri_config::OutputName>()
        .filter(|name| name.serial.is_some())
        .map(|name| name.format_make_model_serial_or_connector())
        .unwrap_or_else(|| output.name())
}

/// Start independently of niri's session-only D-Bus services, including on Winit.
pub fn start(state: &mut State) {
    state.niri.waylandshader.control.start(|source| {
        state
            .niri
            .event_loop
            .insert_source(source, |_, _, state| state.niri.queue_redraw_all())
            .map(|_| ())
            .map_err(|err| err.to_string())
    });
}

render_elements! {
    #[derive(Debug)]
    pub ShaderElement<=GlesRenderer>;
    Runtime=waylandshader_runtime::ShaderElement,
}

impl<'render> RenderElement<TtyRenderer<'render>> for ShaderElement {
    fn capture_framebuffer(
        &self,
        frame: &mut TtyFrame<'_, '_, '_>,
        src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        cache: &UserDataMap,
    ) -> Result<(), TtyRendererError<'render>> {
        RenderElement::<GlesRenderer>::capture_framebuffer(
            self,
            frame.as_gles_frame(),
            src,
            dst,
            cache,
        )?;
        Ok(())
    }
    fn draw(
        &self,
        frame: &mut TtyFrame<'_, '_, '_>,
        src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        opaque: &[Rectangle<i32, Physical>],
        cache: Option<&UserDataMap>,
    ) -> Result<(), TtyRendererError<'render>> {
        RenderElement::<GlesRenderer>::draw(
            self,
            frame.as_gles_frame(),
            src,
            dst,
            damage,
            opaque,
            cache,
        )?;
        Ok(())
    }
}
