//! Niri-specific monitor identity, redraw registration, and renderer adaptation.

pub mod gpu;

use smithay::backend::allocator::dmabuf::Dmabuf;
use smithay::backend::allocator::format::FormatSet;
use smithay::backend::allocator::{Fourcc, Modifier};
use smithay::backend::drm::DrmNode;
use smithay::backend::renderer::element::{render_elements, RenderElement};
use smithay::backend::renderer::gles::{GlesRenderer, GlesTexture};
use smithay::backend::renderer::multigpu::gbm::GbmGlesBackend;
use smithay::backend::renderer::multigpu::{ApiDevice as _, GpuManager, MultiTexture};
use smithay::backend::renderer::{Bind, ContextId, Frame as _, ImportDma, Texture as _};
use smithay::output::Output;
use smithay::utils::user_data::UserDataMap;
use smithay::utils::{Buffer, DeviceFd, Physical, Rectangle, Size};

use crate::backend::tty::{TtyFrame, TtyRenderer, TtyRendererError};
use crate::niri::State;
use crate::render_helpers::renderer::AsGlesFrame as _;

type Api = GbmGlesBackend<GlesRenderer, DeviceFd>;

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

/// Whether Smithay transfers `format` frames of `size` from `render` to `target` by GPU copy.
///
/// Repeats, with public APIs, the checks Smithay's multi-GPU renderer makes before it falls back
/// to CPU copies (`create_shared_dma_framebuffer`; keep in sync when bumping Smithay). It
/// allocates and imports one buffer, so call it only when the format, size or devices change.
pub fn gpu_copy(
    gpu_manager: &mut GpuManager<Api>,
    render: DrmNode,
    target: DrmNode,
    format: Fourcc,
    size: Size<i32, Physical>,
) -> bool {
    let Ok(devices) = gpu_manager.devices_mut() else {
        return false;
    };
    let (mut src, mut dst) = (None, None);
    for device in devices {
        if *device.node() == render {
            src = Some(device);
        } else if *device.node() == target {
            dst = Some(device);
        }
    }
    let (Some(src), Some(dst)) = (src, dst) else {
        return false;
    };
    if !dst.can_do_cross_device_imports() || !src.should_do_cross_device_exports() {
        return false;
    }
    let imports: FormatSet = ImportDma::dmabuf_formats(dst.renderer())
        .iter()
        .filter(|candidate| candidate.code == format)
        .copied()
        .collect();
    let renders = Bind::<Dmabuf>::supported_formats(src.renderer()).unwrap_or_default();
    let modifiers: Vec<_> = imports
        .intersection(&renders)
        .map(|candidate| candidate.modifier)
        .filter(|modifier| *modifier != Modifier::Invalid)
        .collect();
    if modifiers.is_empty() {
        return false;
    }
    let Ok(mut dmabuf) =
        src.allocator()
            .create_buffer(size.w as u32, size.h as u32, format, &modifiers)
    else {
        return false;
    };
    let damage = [Rectangle::from_size((size.w, size.h).into())];
    src.renderer_mut().bind(&mut dmabuf).is_ok()
        && dst
            .renderer_mut()
            .import_dmabuf(&dmabuf, Some(&damage))
            .is_ok()
}

render_elements! {
    #[derive(Debug)]
    pub ShaderElement<=GlesRenderer>;
    Runtime=waylandshader_runtime::ShaderElement,
}

impl ShaderElement {
    fn effect(&self) -> &waylandshader_runtime::ShaderElement {
        match self {
            Self::Runtime(effect) => effect,
            Self::_GenericCatcher(never) => match *never {},
        }
    }
}

/// The shader output as a `MultiTexture` of the render context that created it.
type TtyOutput = (ContextId<GlesTexture>, MultiTexture);

impl<'render> RenderElement<TtyRenderer<'render>> for ShaderElement {
    fn capture_framebuffer(
        &self,
        frame: &mut TtyFrame<'_, '_, '_>,
        src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        cache: &UserDataMap,
    ) -> Result<(), TtyRendererError<'render>> {
        let effect = self.effect();
        RenderElement::<GlesRenderer>::capture_framebuffer(
            effect,
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
        _src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        _opaque: &[Rectangle<i32, Physical>],
        _cache: Option<&UserDataMap>,
    ) -> Result<(), TtyRendererError<'render>> {
        let effect = self.effect();
        // Draw through the MultiFrame, not its inner GLES frame: only tracked damage
        // reaches the target GPU when the output is scanned out from another device.
        let context = frame.as_gles_frame().context_id();
        let texture = effect.with_output(|texture, cache| {
            if let Some((_, wrapped)) = cache
                .as_deref()
                .and_then(|cached| cached.downcast_ref::<TtyOutput>())
                .filter(|(id, _)| *id == context)
            {
                return Some(wrapped.clone());
            }
            let wrapped = MultiTexture::from_native_texture::<Api>(&context, texture.clone())?;
            *cache = Some(Box::new((context, wrapped.clone())));
            Some(wrapped)
        });
        let Some(texture) = texture.flatten() else {
            return Ok(());
        };
        let src = Rectangle::from_size(texture.size().to_f64());
        let transform = frame.transformation().invert();
        frame.render_texture_from_to(&texture, src, dst, damage, &[], transform, 1.)
    }
}

#[cfg(test)]
mod tests {
    use std::fs::{self, File};
    use std::os::fd::OwnedFd;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Arc;
    use std::time::{Duration, Instant};

    use smithay::backend::allocator::dmabuf::{Dmabuf, DmabufAllocator};
    use smithay::backend::allocator::gbm::{GbmAllocator, GbmBufferFlags, GbmDevice};
    use smithay::backend::allocator::{Allocator, Buffer as _, Fourcc, Modifier};
    use smithay::backend::drm::{DrmNode, NodeType};
    use smithay::backend::renderer::element::{Element, RenderElement};
    use smithay::backend::renderer::gles::GlesRenderer;
    use smithay::backend::renderer::multigpu::gbm::GbmGlesBackend;
    use smithay::backend::renderer::multigpu::GpuManager;
    use smithay::backend::renderer::{Bind, Color32F, ExportMem, Frame, Renderer};
    use smithay::output::{Mode, Output, PhysicalProperties, Subpixel};
    use smithay::utils::user_data::UserDataMap;
    use smithay::utils::{DeviceFd, Physical, Rectangle, Size, Transform};
    use tracing::{Event, Level, Subscriber};
    use tracing_subscriber::layer::{Context, Layer, SubscriberExt as _};
    use waylandshader_runtime::{Manager, Transfer};

    use super::{gpu_copy, ShaderElement};
    use crate::backend::tty::TtyRenderer;
    use crate::render_helpers::renderer::{AsGlesFrame as _, AsGlesRenderer as _};

    type Api = GbmGlesBackend<GlesRenderer, DeviceFd>;

    /// Counts Smithay's multi-GPU events: `(info, warnings)`. Smithay warns before every
    /// fallback to CPU copies, which pixels alone cannot reveal.
    #[derive(Clone, Default)]
    struct MultiGpuEvents(Arc<(AtomicUsize, AtomicUsize)>);

    impl<S: Subscriber> Layer<S> for MultiGpuEvents {
        fn on_event(&self, event: &Event<'_>, _: Context<'_, S>) {
            let meta = event.metadata();
            if meta.target() == "smithay::backend::renderer::multigpu" {
                let (info, warnings) = &*self.0;
                if *meta.level() <= Level::WARN {
                    warnings
                } else {
                    info
                }
                .fetch_add(1, Ordering::Relaxed);
            }
        }
    }

    // Inverts even frames, so every frame proves the current shader output was presented.
    const SHADER: &str = r#"#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; uint FrameCount; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = global.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 1) uniform sampler2D Source;
void main() {
    vec3 color = texture(Source, uv).rgb;
    FragColor = vec4(global.FrameCount % 2u == 0u ? vec3(1.0) - color : color, 1.0);
}
"#;

    fn open(path: &str) -> (DrmNode, GbmDevice<DeviceFd>) {
        let node = DrmNode::from_path(path).unwrap();
        assert_eq!(node.ty(), NodeType::Render, "{path} is not a render node");
        let file = File::options().read(true).write(true).open(path).unwrap();
        (
            node,
            GbmDevice::new(DeviceFd::from(OwnedFd::from(file))).unwrap(),
        )
    }

    /// Target-GPU readback, used only as the test oracle.
    fn read(gpu: &mut GpuManager<Api>, node: DrmNode, buffer: &mut Dmabuf) -> Vec<u8> {
        let mut single = gpu.single_renderer(&node).unwrap();
        let renderer = single.as_gles_renderer();
        let size = buffer.size();
        let framebuffer = renderer.bind(buffer).unwrap();
        let mapping = renderer
            .copy_framebuffer(&framebuffer, Rectangle::from_size(size), Fourcc::Abgr8888)
            .unwrap();
        renderer.map_texture(&mapping).unwrap().to_vec()
    }

    /// Draws a left/right split. Only `tracked` clears go through the MultiFrame, like
    /// niri's GLES-only elements that draw on the inner frame.
    fn scene(
        frame: &mut <TtyRenderer<'_> as smithay::backend::renderer::RendererSuper>::Frame<'_, '_>,
        tracked: bool,
    ) {
        let size = frame.output_size();
        let half = Size::from((size.w / 2, size.h));
        let parts = [
            (Rectangle::from_size(half), Color32F::new(0.2, 0.4, 0.6, 1.)),
            (
                Rectangle::new((half.w, 0).into(), (size.w - half.w, size.h).into()),
                Color32F::new(0.8, 0.1, 0.3, 1.),
            ),
        ];
        for (rect, color) in parts {
            if tracked {
                frame.clear(color, &[rect]).unwrap();
            } else {
                frame.as_gles_frame().clear(color, &[rect]).unwrap();
            }
        }
    }

    fn ready(manager: &mut Manager, renderer: &mut GlesRenderer, output: &Output) -> ShaderElement {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            if let Some(effect) = manager.prepare(renderer, output, true, Some(Transfer::GpuCopy)) {
                return ShaderElement::from(effect);
            }
            assert!(Instant::now() < deadline, "shader did not compile");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    #[test]
    #[ignore = "needs two GPUs: WAYLANDSHADER_TEST_RENDER_NODES='/dev/dri/renderD128 /dev/dri/renderD129'"]
    fn cross_gpu_presentation() {
        let events = MultiGpuEvents::default();
        let _events =
            tracing::subscriber::set_default(tracing_subscriber::registry().with(events.clone()));
        let nodes = std::env::var("WAYLANDSHADER_TEST_RENDER_NODES")
            .expect("set WAYLANDSHADER_TEST_RENDER_NODES to two render node paths");
        let nodes: Vec<_> = nodes.split_whitespace().collect();
        assert!(
            nodes.len() == 2 && nodes[0] != nodes[1],
            "two distinct render nodes"
        );
        let root =
            std::env::temp_dir().join(format!("waylandshader-cross-gpu-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("invert.slang"), SHADER).unwrap();
        fs::write(
            root.join("invert.slangp"),
            "shaders = 1\nshader0 = invert.slang\n",
        )
        .unwrap();
        let settings = serde_json::json!({"enabled": true, "preset": root.join("invert.slangp")});
        fs::write(root.join("settings.json"), settings.to_string()).unwrap();
        // Keep compiler caches and settings out of the user's directories.
        std::env::set_var("WAYLANDSHADER_CONFIG", root.join("settings.json"));
        std::env::set_var("XDG_CACHE_HOME", &root);

        let (first, first_gbm) = open(nodes[0]);
        let (second, second_gbm) = open(nodes[1]);
        let mut api = Api::default();
        api.add_node(first, first_gbm.clone()).unwrap();
        api.add_node(second, second_gbm.clone()).unwrap();
        let mut gpu = GpuManager::new(api).unwrap();
        let mut manager = Manager::new(|output| output.name());
        let output = Output::new(
            "test".into(),
            PhysicalProperties {
                size: (0, 0).into(),
                subpixel: Subpixel::Unknown,
                make: "test".into(),
                model: "test".into(),
                serial_number: "test".into(),
            },
        );
        let mode = |size: (i32, i32)| Mode {
            size: size.into(),
            refresh: 60_000,
        };
        manager.add_output(&output);
        let cache = UserDataMap::new();

        for (render, target, target_gbm) in
            [(first, second, &second_gbm), (second, first, &first_gbm)]
        {
            let mut allocator = DmabufAllocator(GbmAllocator::new(
                target_gbm.clone(),
                GbmBufferFlags::RENDERING,
            ));
            for format in [Fourcc::Abgr8888, Fourcc::Argb2101010] {
                for transform in [Transform::Normal, Transform::_90, Transform::Flipped180] {
                    for index in 0..4 {
                        let size: Size<i32, Physical> =
                            if index < 2 { (64, 48) } else { (80, 40) }.into();
                        output.change_current_state(
                            Some(mode((size.w, size.h))),
                            Some(transform),
                            None,
                            None,
                        );
                        let mut buffer = allocator
                            .create_buffer(
                                size.w as u32,
                                size.h as u32,
                                format,
                                &[Modifier::Linear],
                            )
                            .unwrap();

                        // Reference: the unfiltered scene through tracked MultiFrame damage.
                        {
                            let mut renderer = gpu.renderer(&render, &target, format).unwrap();
                            let mut framebuffer = renderer.bind(&mut buffer).unwrap();
                            let mut frame =
                                renderer.render(&mut framebuffer, size, transform).unwrap();
                            scene(&mut frame, true);
                            frame.finish().unwrap().wait().unwrap();
                        }
                        let reference = read(&mut gpu, target, &mut buffer);

                        // Clear the target so only transferred damage can change it.
                        {
                            let mut single = gpu.single_renderer(&target).unwrap();
                            let renderer = single.as_gles_renderer();
                            let mut framebuffer = renderer.bind(&mut buffer).unwrap();
                            let mut frame = renderer
                                .render(&mut framebuffer, size, Transform::Normal)
                                .unwrap();
                            frame
                                .clear(Color32F::new(1., 0., 1., 1.), &[Rectangle::from_size(size)])
                                .unwrap();
                            frame.finish().unwrap().wait().unwrap();
                        }

                        assert!(
                            gpu_copy(&mut gpu, render, target, format, size),
                            "{render:?} -> {target:?} {format:?} needs a GPU-copy transfer"
                        );
                        let mut renderer = gpu.renderer(&render, &target, format).unwrap();
                        let effect = ready(&mut manager, renderer.as_gles_renderer(), &output);
                        let mut framebuffer = renderer.bind(&mut buffer).unwrap();
                        let mut frame = renderer.render(&mut framebuffer, size, transform).unwrap();
                        scene(&mut frame, false);
                        let full = Rectangle::from_size(frame.output_size());
                        let src = effect.src();
                        RenderElement::<TtyRenderer<'_>>::capture_framebuffer(
                            &effect, &mut frame, src, full, &cache,
                        )
                        .unwrap();
                        RenderElement::<TtyRenderer<'_>>::draw(
                            &effect,
                            &mut frame,
                            src,
                            full,
                            &[full],
                            &[],
                            Some(&cache),
                        )
                        .unwrap();
                        frame.finish().unwrap().wait().unwrap();
                        drop(framebuffer);
                        drop(renderer);

                        // Geometry changes restart the shader's frame count.
                        let inverted = index % 2 == 0;
                        let presented = read(&mut gpu, target, &mut buffer);
                        for (pixel, (actual, expected)) in presented
                            .chunks_exact(4)
                            .zip(reference.chunks_exact(4))
                            .enumerate()
                        {
                            for channel in 0..4 {
                                let expected = if inverted && channel < 3 {
                                    255 - expected[channel]
                                } else {
                                    expected[channel]
                                };
                                assert!(
                                    actual[channel].abs_diff(expected) <= 2,
                                    "{render:?} -> {target:?} {format:?} {transform:?} frame {index} \
                                     pixel {pixel}: {actual:?}, reference {:?}",
                                    &reference[pixel * 4..pixel * 4 + 4],
                                );
                            }
                        }
                    }
                }
            }
        }

        // Smithay logs each shared transfer buffer it allocates, and warns before it falls
        // back to CPU copies: gpu_copy() predicted GPU copies, so none may have occurred.
        let (info, warnings) = &*events.0;
        assert!(
            info.load(Ordering::Relaxed) > 0,
            "Smithay's multi-GPU log was not observed"
        );
        assert_eq!(
            warnings.load(Ordering::Relaxed),
            0,
            "Smithay fell back to CPU copies that gpu_copy() did not predict"
        );

        // Frames that only reach the output's GPU by CPU copy stay unfiltered and idle.
        let mut renderer = gpu.renderer(&first, &second, Fourcc::Abgr8888).unwrap();
        ready(&mut manager, renderer.as_gles_renderer(), &output);
        let cpu = Some(Transfer::CpuCopy);
        assert!(manager
            .prepare(renderer.as_gles_renderer(), &output, true, cpu)
            .is_none());
        assert!(!manager.animated(&output));

        drop(renderer);
        manager.release_all();
        drop(manager);
        drop(gpu);
        let _ = fs::remove_dir_all(root);
    }
}
