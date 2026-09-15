use std::collections::BTreeMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::ptr::NonNull;

use smithay::backend::egl::EGLDisplay;

#[repr(C)]
struct Parameter {
    name: *const c_char,
    value: f32,
}

unsafe extern "C" {
    fn ws_create(
        display: *mut c_void,
        preset: *const c_char,
        parameters: *const Parameter,
        count: usize,
        error: *mut c_char,
        error_size: usize,
    ) -> *mut c_void;
    fn ws_destroy(runtime: *mut c_void);
    fn ws_resize(
        runtime: *mut c_void,
        width: u32,
        height: u32,
        error: *mut c_char,
        error_size: usize,
    ) -> bool;
    fn ws_input_image(runtime: *const c_void) -> *mut c_void;
    fn ws_output_image(runtime: *const c_void) -> *mut c_void;
    fn ws_render(
        runtime: *mut c_void,
        frame: u64,
        fps: f32,
        elapsed: u32,
        clear: bool,
        shader: bool,
        gamma: f32,
        saturation: f32,
        error: *mut c_char,
        error_size: usize,
    ) -> bool;
    fn ws_set_parameter(
        runtime: *mut c_void,
        name: *const c_char,
        value: f32,
        error: *mut c_char,
        error_size: usize,
    ) -> bool;
    fn ws_parameters_json(runtime: *const c_void) -> *const c_char;
}

// The private context is released by every ABI call. Ownership moves from the
// compiler thread to the compositor; no instance is ever accessed concurrently.
#[derive(Debug)]
pub struct Runtime {
    raw: NonNull<c_void>,
    _display: EGLDisplay,
    parameters: serde_json::Value,
}
unsafe impl Send for Runtime {}

fn error_text(error: &[c_char]) -> String {
    unsafe { CStr::from_ptr(error.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

impl Runtime {
    pub fn create(
        display: EGLDisplay,
        preset: &str,
        values: &BTreeMap<String, f32>,
    ) -> Result<Self, String> {
        let preset = CString::new(preset).map_err(|_| "Preset path contains NUL")?;
        let names = values
            .keys()
            .map(|name| CString::new(name.as_str()))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| "Parameter name contains NUL")?;
        let parameters = names
            .iter()
            .zip(values.values())
            .map(|(name, value)| Parameter {
                name: name.as_ptr(),
                value: *value,
            })
            .collect::<Vec<_>>();
        let mut error = [0; 4096];
        let raw = unsafe {
            ws_create(
                (**display.get_display_handle()).cast_mut(),
                preset.as_ptr(),
                parameters.as_ptr(),
                parameters.len(),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        let raw = NonNull::new(raw).ok_or_else(|| error_text(&error))?;
        let mut runtime = Self {
            raw,
            _display: display,
            parameters: serde_json::Value::Null,
        };
        runtime.refresh_parameters()?;
        Ok(runtime)
    }

    pub fn resize(&mut self, width: i32, height: i32) -> Result<[*mut c_void; 2], String> {
        let mut error = [0; 4096];
        if !unsafe {
            ws_resize(
                self.raw.as_ptr(),
                width as u32,
                height as u32,
                error.as_mut_ptr(),
                error.len(),
            )
        } {
            return Err(error_text(&error));
        }
        Ok(unsafe {
            [
                ws_input_image(self.raw.as_ptr()),
                ws_output_image(self.raw.as_ptr()),
            ]
        })
    }

    pub fn render(
        &mut self,
        frame: u64,
        fps: f32,
        elapsed: u32,
        clear: bool,
        shader: bool,
        gamma: f32,
        saturation: f32,
    ) -> Result<(), String> {
        let mut error = [0; 4096];
        if unsafe {
            ws_render(
                self.raw.as_ptr(),
                frame,
                fps,
                elapsed,
                clear,
                shader,
                gamma,
                saturation,
                error.as_mut_ptr(),
                error.len(),
            )
        } {
            Ok(())
        } else {
            Err(error_text(&error))
        }
    }

    pub fn set_parameter(&mut self, name: &str, value: f32) -> Result<(), String> {
        let name = CString::new(name).map_err(|_| "Parameter name contains NUL")?;
        let mut error = [0; 4096];
        if !unsafe {
            ws_set_parameter(
                self.raw.as_ptr(),
                name.as_ptr(),
                value,
                error.as_mut_ptr(),
                error.len(),
            )
        } {
            return Err(error_text(&error));
        }
        self.refresh_parameters()
    }

    pub fn parameters(&self) -> serde_json::Value {
        self.parameters.clone()
    }

    fn refresh_parameters(&mut self) -> Result<(), String> {
        let json = unsafe { ws_parameters_json(self.raw.as_ptr()) };
        if json.is_null() {
            return Err("Shader runtime returned no parameter metadata".into());
        }
        let parameters: serde_json::Value =
            serde_json::from_slice(unsafe { CStr::from_ptr(json) }.to_bytes())
                .map_err(|error| format!("Invalid shader parameter metadata: {error}"))?;
        if !parameters.is_array() {
            return Err("Shader parameter metadata is not an array".into());
        }
        self.parameters = parameters;
        Ok(())
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        unsafe { ws_destroy(self.raw.as_ptr()) };
    }
}
