//! Source-linked shader runtime with private native/EGL ownership.
//!
//! The host supplies output-profile identity and control-worker redraw registration;
//! renderer-specific adapters and compositor lifecycle policy remain with the host.

mod bridge;
pub mod control;

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::thread::{self, JoinHandle};
use std::time::Instant;

use smithay::backend::renderer::element::{Element, Id, RenderElement};
use smithay::backend::renderer::gles::{ffi, GlesError, GlesFrame, GlesRenderer, GlesTexture};
use smithay::backend::renderer::utils::CommitCounter;
use smithay::backend::renderer::{Frame as _, FrameContext, Texture as _};
use smithay::output::Output;
use smithay::utils::user_data::UserDataMap;
use smithay::utils::{Buffer, Physical, Rectangle, Scale, Size, Transform};

use bridge::Runtime;
use control::{Control, OutputSettings, Settings};

type Compilation = Result<Runtime, String>;

struct OutputRuntime {
    id: Id,
    profile: String,
    commit: CommitCounter,
    live: Rc<RefCell<Live>>,
    worker: Option<JoinHandle<()>>,
    result: Option<Receiver<Compilation>>,
    candidate: Option<Compilation>,
    attempted: Option<u64>,
    context: usize,
    geometry: Option<(Size<i32, Physical>, Transform, f64)>,
    supported: bool,
}

struct Live {
    // Siblings are released before the desktop storage/context.
    textures: Option<[GlesTexture; 2]>,
    runtime: Option<Runtime>,
    size: Size<i32, Buffer>,
    options: OutputSettings,
    shader: bool,
    active: bool,
    ready: bool,
    clear: bool,
    frame: u64,
    start: Instant,
    fps: f32,
    error: String,
    dirty: bool,
}

impl Live {
    fn reset(&mut self) {
        self.ready = false;
        self.clear = true;
        self.frame = 0;
        self.start = Instant::now();
        self.dirty = true;
    }

    fn retire(&mut self) {
        self.textures = None;
        self.runtime = None;
        self.reset();
    }
}

impl OutputRuntime {
    fn new(profile: String) -> Self {
        Self {
            profile,
            id: Id::new(),
            commit: CommitCounter::default(),
            worker: None,
            result: None,
            candidate: None,
            attempted: None,
            context: 0,
            geometry: None,
            supported: true,
            live: Rc::new(RefCell::new(Live {
                textures: None,
                runtime: None,
                size: (0, 0).into(),
                options: OutputSettings::default(),
                shader: false,
                active: false,
                ready: false,
                clear: true,
                frame: 0,
                start: Instant::now(),
                fps: 60.,
                error: String::new(),
                dirty: true,
            })),
        }
    }

    fn release(&mut self) {
        // Only lifecycle teardown waits: never compilation or a frame path.
        if let Some(worker) = self.worker.take() {
            drop(worker.join());
        }
        self.result = None;
        self.candidate = None;
        self.live.borrow_mut().retire();
        self.attempted = None;
        self.context = 0;
    }
}

impl Drop for OutputRuntime {
    fn drop(&mut self) {
        self.release();
    }
}

/// Coordinates transactional shader replacement and per-output GLES presentation.
pub struct Manager {
    pub control: Control,
    settings: Settings,
    outputs: BTreeMap<String, OutputRuntime>,
    profile_for_output: fn(&Output) -> String,
    committed: String,
    loading: bool,
    preset_failed: bool,
    dirty: bool,
    locked: bool,
    error: String,
}

impl Manager {
    /// Creates a manager whose profile resolver runs only when an output runtime
    /// is first registered, including lazy registration by [`Self::prepare`].
    pub fn new(profile_for_output: fn(&Output) -> String) -> Self {
        let control = Control::new();
        let settings = control
            .snapshot_since(u64::MAX)
            .expect("initial shader settings");
        Self {
            control,
            settings,
            outputs: BTreeMap::new(),
            profile_for_output,
            committed: String::new(),
            loading: false,
            preset_failed: false,
            dirty: true,
            locked: false,
            error: String::new(),
        }
    }

    pub fn add_output(&mut self, output: &Output) {
        self.outputs
            .entry(output.name())
            .or_insert_with(|| OutputRuntime::new((self.profile_for_output)(output)));
        self.dirty = true;
    }

    pub fn remove_output(&mut self, output: &Output) {
        self.outputs.remove(&output.name());
        if self.outputs.is_empty() && self.loading {
            self.loading = false;
            self.preset_failed = true;
            self.error = "All outputs disconnected while loading the shader preset".into();
        }
        self.dirty = true;
        self.publish();
        self.control.wake();
    }

    pub fn release_all(&mut self) {
        for output in self.outputs.values_mut() {
            output.release();
        }
        self.loading = false;
        self.dirty = true;
        self.publish();
    }

    pub fn invalidate_history(&mut self) {
        for output in self.outputs.values() {
            output.live.borrow_mut().reset();
        }
    }

    fn refresh(&mut self) {
        if let Some(settings) = self.control.snapshot_since(self.settings.generation) {
            if settings.preset_generation != self.settings.preset_generation {
                // In-flight workers are allowed to finish, but their generation is
                // discarded before a new request starts; never join on a redraw.
                self.loading = true;
                self.error.clear();
                self.preset_failed = false;
            }
            if settings.enabled != self.settings.enabled {
                self.invalidate_history();
            }
            if settings.preset == self.settings.preset
                && settings.parameters != self.settings.parameters
            {
                self.error.clear();
                self.preset_failed = false;
            }
            for output in self.outputs.values() {
                let mut live = output.live.borrow_mut();
                let options = settings
                    .outputs
                    .get(&output.profile)
                    .cloned()
                    .unwrap_or_default();
                if options.shader_enabled != live.options.shader_enabled {
                    live.reset();
                }
                live.options = options;
                if let Some(runtime) = &mut live.runtime {
                    for (name, value) in &settings.parameters {
                        if self.settings.parameters.get(name) != Some(value) {
                            if let Err(error) = runtime.set_parameter(name, *value) {
                                self.error = error;
                            }
                        }
                    }
                }
            }
            self.settings = settings;
            self.dirty = true;
        }
        for output in self.outputs.values_mut() {
            let result = output
                .result
                .as_ref()
                .and_then(|receiver| match receiver.try_recv() {
                    Ok(result) => Some(result),
                    Err(TryRecvError::Empty) => None,
                    Err(TryRecvError::Disconnected) => {
                        Some(Err("Shader compiler worker panicked".into()))
                    }
                });
            if let Some(result) = result {
                // Sending the result relinquishes all GPU ownership on the worker.
                output.worker = None;
                output.result = None;
                if output.attempted == Some(self.settings.preset_generation) {
                    output.candidate = Some(result);
                } else {
                    drop(result);
                    output.attempted = None;
                }
                self.dirty = true;
            }
            self.dirty |= std::mem::take(&mut output.live.borrow_mut().dirty);
        }
        // A preset replacement commits across all connected supported outputs,
        // not whichever worker finishes first. Any compile error preserves every
        // old chain and its parameter metadata.
        let complete =
            self.outputs.values().filter(|o| o.supported).all(|o| {
                o.attempted == Some(self.settings.preset_generation) && o.worker.is_none()
            });
        if complete && self.outputs.values().any(|o| o.candidate.is_some()) {
            let failure = self
                .outputs
                .values()
                .find_map(|o| o.candidate.as_ref().and_then(|r| r.as_ref().err()).cloned());
            if let Some(error) = failure {
                self.error = error;
                self.preset_failed = true;
                for output in self.outputs.values_mut() {
                    output.candidate = None;
                }
            } else {
                self.preset_failed = false;
                self.error.clear();
                for output in self.outputs.values_mut() {
                    if let Some(Ok(mut runtime)) = output.candidate.take() {
                        for (name, value) in &self.settings.parameters {
                            if let Err(error) = runtime.set_parameter(name, *value) {
                                self.error = error;
                            }
                        }
                        let mut live = output.live.borrow_mut();
                        live.retire();
                        live.runtime = Some(runtime);
                        live.shader = !self.settings.preset.is_empty();
                        live.error.clear();
                    }
                }
                self.committed = self.settings.preset.clone();
            }
            self.loading = false;
            self.dirty = true;
        }
    }

    pub fn prepare(
        &mut self,
        renderer: &mut GlesRenderer,
        output: &Output,
        unlocked: bool,
        same_gpu: bool,
    ) -> Option<ShaderElement> {
        self.refresh();
        if self.locked == unlocked {
            self.locked = !unlocked;
            self.invalidate_history();
        }
        let name = output.name();
        let state = self
            .outputs
            .entry(name)
            .or_insert_with(|| OutputRuntime::new((self.profile_for_output)(output)));
        let context = renderer.egl_context().get_context_handle() as usize;
        if state.context != 0 && state.context != context {
            state.release();
        }
        state.context = context;
        state.supported = same_gpu;
        let options = self
            .settings
            .outputs
            .get(&state.profile)
            .cloned()
            .unwrap_or_default();
        let size = output
            .current_transform()
            .transform_size(output.current_mode()?.size)
            .to_logical(1)
            .to_physical(1);
        let geometry = (
            size,
            output.current_transform(),
            output.current_scale().fractional_scale(),
        );
        let mut live = state.live.borrow_mut();
        if state.geometry != Some(geometry) {
            state.geometry = Some(geometry);
            live.textures = None;
            live.reset();
        }
        live.options = options;
        live.fps = output
            .current_mode()
            .map_or(60., |mode| mode.refresh as f32 / 1000.);
        let active = unlocked
            && same_gpu
            && self.settings.enabled
            && ((live.shader && live.options.shader_enabled) || live.options.color_enabled);
        if live.active != active {
            live.active = active;
            live.reset();
        }
        if !same_gpu && self.settings.enabled {
            live.error =
                "Shader presentation is unsupported when target and render GPUs differ".into();
            self.dirty = true;
        }
        drop(live);
        if same_gpu
            && state.worker.is_none()
            && state.attempted != Some(self.settings.preset_generation)
        {
            let display = renderer.egl_context().display().clone();
            let preset = self.settings.preset.clone();
            let parameters = self.settings.parameters.clone();
            let control = self.control.clone();
            state.attempted = Some(self.settings.preset_generation);
            state.candidate = None;
            let (sender, receiver) = mpsc::channel();
            state.result = Some(receiver);
            state.worker = Some(thread::spawn(move || {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    Runtime::create(display, &preset, &parameters)
                }))
                .unwrap_or_else(|_| Err("Shader compiler worker panicked".into()));
                let _ = sender.send(result);
                control.wake();
            }));
            self.loading = true;
            self.dirty = true;
        }
        let live = state.live.borrow();
        let element = if live.active && live.runtime.is_some() {
            state.commit.increment();
            Some(ShaderElement {
                id: state.id.clone(),
                commit: state.commit,
                size,
                live: state.live.clone(),
                control: self.control.clone(),
            })
        } else {
            None
        };
        drop(live);
        if self.outputs.values().all(|state| !state.supported) {
            self.loading = false;
            self.preset_failed = true;
            self.error = "Shader presentation requires an output on the render GPU".into();
            self.dirty = true;
        }
        self.publish();
        element
    }

    pub fn animated(&self, output: &Output) -> bool {
        self.outputs.get(&output.name()).is_some_and(|state| {
            let live = state.live.borrow();
            live.active && live.runtime.is_some() && live.shader && live.options.shader_enabled
        })
    }

    fn publish(&mut self) {
        if !self.dirty {
            return;
        }
        self.dirty = false;
        let parameters = self
            .outputs
            .values()
            .find_map(|state| {
                let live = state.live.borrow();
                live.runtime.as_ref().map(Runtime::parameters)
            })
            .unwrap_or_else(|| serde_json::json!([]));
        let mut active = false;
        let mut error = self.error.clone();
        let outputs = self.outputs.iter().map(|(name, state)| {
            let live = state.live.borrow();
            let shader = live.active && live.ready && live.shader && live.options.shader_enabled;
            let color = live.active && live.ready && live.options.color_enabled;
            active |= shader || color;
            if error.is_empty() && !live.error.is_empty() { error = live.error.clone(); }
            serde_json::json!({"id":state.profile,"name":name,"shaderEnabled":live.options.shader_enabled,
                "colorEnabled":live.options.color_enabled,"gamma":live.options.gamma,
                "saturation":live.options.saturation,"shaderActive":shader,"colorActive":color})
        }).collect::<Vec<_>>();
        self.control.publish(serde_json::json!({"generation":self.settings.generation,
            "presetGeneration":self.settings.preset_generation,"enabled":self.settings.enabled,
            "presetFailed":self.preset_failed,
            "active":active,"loading":self.loading,"preset":self.committed,
            "requestedPreset":self.settings.preset,"error":error,"parameters":parameters,"outputs":outputs}));
    }
}

/// A GLES framebuffer effect; host renderers may wrap it with static delegation.
pub struct ShaderElement {
    id: Id,
    commit: CommitCounter,
    size: Size<i32, Physical>,
    live: Rc<RefCell<Live>>,
    control: Control,
}

impl std::fmt::Debug for ShaderElement {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ShaderElement")
            .field("id", &self.id)
            .field("size", &self.size)
            .finish()
    }
}

impl Element for ShaderElement {
    fn id(&self) -> &Id {
        &self.id
    }
    fn current_commit(&self) -> CommitCounter {
        self.commit
    }
    fn src(&self) -> Rectangle<f64, Buffer> {
        Rectangle::from_size(
            self.size
                .to_f64()
                .to_logical(1.)
                .to_buffer(1., Transform::Normal),
        )
    }
    fn geometry(&self, _scale: Scale<f64>) -> Rectangle<i32, Physical> {
        Rectangle::from_size(self.size)
    }
    fn is_framebuffer_effect(&self) -> bool {
        true
    }
}

impl ShaderElement {
    fn capture(&self, frame: &mut GlesFrame<'_, '_>) -> Result<(), String> {
        let mut live = self.live.borrow_mut();
        live.ready = false;
        let size = frame
            .transformation()
            .transform_size(frame.output_size())
            .to_logical(1)
            .to_buffer(1, Transform::Normal);
        if live.textures.is_none() || live.size != size {
            live.textures = None;
            live.reset();
            let images = frame
                .with_context(|_| live.runtime.as_mut().unwrap().resize(size.w, size.h))
                .map_err(|e| e.to_string())??;
            let names = frame
                .with_context(|gl| unsafe {
                    let import =
                        smithay::backend::egl::get_proc_address("glEGLImageTargetTexture2DOES");
                    if import.is_null() {
                        return Err("Missing glEGLImageTargetTexture2DOES".to_owned());
                    }
                    let import: unsafe extern "system" fn(u32, *mut std::ffi::c_void) =
                        std::mem::transmute(import);
                    let mut old_texture = 0;
                    gl.GetIntegerv(ffi::TEXTURE_BINDING_2D, &mut old_texture);
                    let mut names = [0; 2];
                    gl.GenTextures(2, names.as_mut_ptr());
                    for (texture, image) in names.iter().zip(images) {
                        gl.BindTexture(ffi::TEXTURE_2D, *texture);
                        import(ffi::TEXTURE_2D, image);
                        gl.TexParameteri(
                            ffi::TEXTURE_2D,
                            ffi::TEXTURE_MIN_FILTER,
                            ffi::LINEAR as i32,
                        );
                        gl.TexParameteri(
                            ffi::TEXTURE_2D,
                            ffi::TEXTURE_MAG_FILTER,
                            ffi::LINEAR as i32,
                        );
                        gl.TexParameteri(
                            ffi::TEXTURE_2D,
                            ffi::TEXTURE_WRAP_S,
                            ffi::CLAMP_TO_EDGE as i32,
                        );
                        gl.TexParameteri(
                            ffi::TEXTURE_2D,
                            ffi::TEXTURE_WRAP_T,
                            ffi::CLAMP_TO_EDGE as i32,
                        );
                        gl.TexParameteri(ffi::TEXTURE_2D, ffi::TEXTURE_MAX_LEVEL, 0);
                    }
                    gl.BindTexture(ffi::TEXTURE_2D, old_texture as u32);
                    if gl.GetError() != ffi::NO_ERROR {
                        gl.DeleteTextures(2, names.as_ptr());
                        Err("Failed importing shader EGLImage siblings".to_owned())
                    } else {
                        Ok(names)
                    }
                })
                .map_err(|e| e.to_string())??;
            let mut guard = frame.renderer();
            let renderer = guard.as_mut();
            live.textures = Some(names.map(|name| unsafe {
                GlesTexture::from_raw(renderer, Some(ffi::RGBA8), true, name, size)
            }));
            live.size = size;
        }
        let input = live.textures.as_ref().unwrap()[0].tex_id();
        frame
            .with_context(|gl| unsafe {
                let mut draw = 0;
                let mut read = 0;
                gl.GetIntegerv(ffi::DRAW_FRAMEBUFFER_BINDING, &mut draw);
                gl.GetIntegerv(ffi::READ_FRAMEBUFFER_BINDING, &mut read);
                let scissor = gl.IsEnabled(ffi::SCISSOR_TEST) == ffi::TRUE;
                gl.Disable(ffi::SCISSOR_TEST);
                let mut fbo = 0;
                gl.GenFramebuffers(1, &mut fbo);
                gl.BindFramebuffer(ffi::READ_FRAMEBUFFER, draw as u32);
                gl.BindFramebuffer(ffi::DRAW_FRAMEBUFFER, fbo);
                gl.FramebufferTexture2D(
                    ffi::DRAW_FRAMEBUFFER,
                    ffi::COLOR_ATTACHMENT0,
                    ffi::TEXTURE_2D,
                    input,
                    0,
                );
                let complete =
                    gl.CheckFramebufferStatus(ffi::DRAW_FRAMEBUFFER) == ffi::FRAMEBUFFER_COMPLETE;
                if complete {
                    gl.BlitFramebuffer(
                        0,
                        0,
                        size.w,
                        size.h,
                        0,
                        0,
                        size.w,
                        size.h,
                        ffi::COLOR_BUFFER_BIT,
                        ffi::NEAREST,
                    );
                }
                gl.BindFramebuffer(ffi::DRAW_FRAMEBUFFER, draw as u32);
                gl.BindFramebuffer(ffi::READ_FRAMEBUFFER, read as u32);
                if scissor {
                    gl.Enable(ffi::SCISSOR_TEST);
                }
                gl.DeleteFramebuffers(1, &fbo);
                if !complete || gl.GetError() != ffi::NO_ERROR {
                    Err("Shader framebuffer capture failed".to_owned())
                } else {
                    Ok(())
                }
            })
            .map_err(|e| e.to_string())??;
        let frame_number = live.frame;
        let fps = live.fps;
        let elapsed = live.start.elapsed().as_millis().min(u32::MAX as u128) as u32;
        let clear = live.clear;
        let shader = live.shader && live.options.shader_enabled;
        let gamma = if live.options.color_enabled {
            live.options.gamma
        } else {
            1.
        };
        let saturation = if live.options.color_enabled {
            live.options.saturation
        } else {
            1.
        };
        frame
            .with_context(|_| {
                live.runtime.as_mut().unwrap().render(
                    frame_number,
                    fps,
                    elapsed,
                    clear,
                    shader,
                    gamma,
                    saturation,
                )
            })
            .map_err(|e| e.to_string())??;
        live.ready = true;
        live.clear = false;
        live.frame = live.frame.wrapping_add(1);
        live.start = Instant::now();
        if frame_number == 0 {
            live.dirty = true;
            self.control.wake();
        }
        Ok(())
    }
}

impl RenderElement<GlesRenderer> for ShaderElement {
    fn capture_framebuffer(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        _src: Rectangle<f64, Buffer>,
        _dst: Rectangle<i32, Physical>,
        _cache: &UserDataMap,
    ) -> Result<(), GlesError> {
        if let Err(error) = self.capture(frame) {
            let mut live = self.live.borrow_mut();
            live.retire();
            live.error = error;
            self.control.wake();
            // The current scene remains intact; never draw an old filtered frame.
        }
        Ok(())
    }

    fn draw(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        _src: Rectangle<f64, Buffer>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        _opaque: &[Rectangle<i32, Physical>],
        _cache: Option<&UserDataMap>,
    ) -> Result<(), GlesError> {
        let live = self.live.borrow();
        if !live.ready {
            return Ok(());
        }
        let texture = &live.textures.as_ref().unwrap()[1];
        frame.render_texture_from_to(
            texture,
            Rectangle::from_size(texture.size().to_f64()),
            dst,
            damage,
            &[],
            frame.transformation().invert(),
            1.,
            None,
            &[],
        )
    }
}
