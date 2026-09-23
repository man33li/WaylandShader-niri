//! Render GPU status and the controller's next-login render GPU preference.
//!
//! niri selects its composition GPU once, at startup, from `debug { render-drm-device }`.
//! The controller writes that setting only into a WaylandShader-owned file which the user
//! explicitly includes, validates the resulting configuration, and reports the preference
//! separately from the device this session actually uses.

use std::collections::BTreeMap;
use std::ffi::CStr;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write as _};
use std::os::unix::fs::OpenOptionsExt as _;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError, Weak};

use niri_config::{Config, ConfigPath};
use serde_json::json;
use smithay::backend::drm::DrmNode;
use smithay::backend::renderer::gles::{ffi, GlesRenderer};
use smithay::reexports::rustix::fs::{access, Access};
use waylandshader_runtime::control::Control;

/// The managed include, next to the niri configuration file that includes it.
const FILE: &str = "waylandshader-gpu.kdl";
const HEADER: &str =
    "// Managed by the WaylandShader controller: niri's render GPU from the next login.\n";

#[derive(Clone)]
pub struct Gpu(Arc<Mutex<State>>);

struct State {
    control: Control,
    // Reported by the TTY backend on the compositor thread.
    active: Option<PathBuf>,
    startup: Option<PathBuf>,
    fallback: bool,
    devices: BTreeMap<PathBuf, Device>,
    outputs: BTreeMap<String, PathBuf>,
    // Reported after configuration loads on the watcher and control threads.
    config: Option<PathBuf>,
    configured: Option<PathBuf>,
    preference: (&'static str, String),
}

struct Device {
    /// Stable `/dev/dri/by-path` identity when available.
    path: PathBuf,
    name: String,
}

impl Gpu {
    pub fn new(control: &Control) -> Self {
        let gpu = Self(Arc::new(Mutex::new(State {
            control: control.clone(),
            active: None,
            startup: None,
            fallback: false,
            devices: BTreeMap::new(),
            outputs: BTreeMap::new(),
            config: None,
            configured: None,
            preference: ("unavailable", String::new()),
        })));
        // The control service owns the handler; a weak link avoids a reference cycle.
        let weak: Weak<Mutex<State>> = Arc::downgrade(&gpu.0);
        control.set_render_device_handler(move |device| match weak.upgrade() {
            Some(state) => Gpu(state).save(device),
            None => Err("niri is shutting down".to_owned()),
        });
        gpu
    }

    /// Records the render GPU this session uses and the startup request it came from.
    pub fn set_active(&self, active: DrmNode, requested: Option<PathBuf>, fallback: bool) {
        self.update(|state| {
            state.active = active.dev_path();
            state.startup = requested;
            state.fallback = fallback;
        });
    }

    pub fn add_device(&self, node: DrmNode, renderer: &mut GlesRenderer) {
        let Some(node) = node.dev_path() else {
            return;
        };
        let path = stable_path(&node).unwrap_or_else(|| node.clone());
        let name = renderer
            .with_context(|gl| unsafe {
                let name = gl.GetString(ffi::RENDERER);
                (!name.is_null())
                    .then(|| CStr::from_ptr(name.cast()).to_string_lossy().into_owned())
            })
            .ok()
            .flatten()
            .unwrap_or_default();
        // Drop driver details such as "(radeonsi, navi23, ACO, DRM 3.64, …)".
        let name = name.split(" (").next().unwrap_or_default().to_owned();
        self.update(|state| {
            state.devices.insert(node, Device { path, name });
        });
    }

    pub fn remove_device(&self, node: DrmNode) {
        if let Some(node) = node.dev_path() {
            self.update(|state| {
                state.devices.remove(&node);
            });
        }
    }

    /// Records which render GPU drives each connected output.
    pub fn set_outputs(&self, outputs: impl IntoIterator<Item = (String, DrmNode)>) {
        let outputs = outputs
            .into_iter()
            .filter_map(|(name, node)| Some((name, node.dev_path()?)))
            .collect();
        self.update(|state| state.outputs = outputs);
    }

    /// Records a loaded configuration. Performs small file checks, so call it off the
    /// compositor thread except once at startup. `configured` is `None` when the
    /// configuration failed to parse and niri kept the previous one.
    pub fn config_loaded(
        &self,
        path: &ConfigPath,
        includes: &[PathBuf],
        configured: Option<Option<&Path>>,
    ) {
        let config = match path {
            ConfigPath::Explicit(path) => Some(path.clone()),
            ConfigPath::Regular {
                user_path,
                system_path,
            } => [user_path, system_path]
                .into_iter()
                .find(|path| path.exists())
                .cloned(),
        };
        let preference = preference(config.as_deref(), includes);
        self.update(|state| {
            state.config = config;
            if let Some(configured) = configured {
                state.configured = configured.map(Path::to_path_buf);
            }
            state.preference = preference;
        });
    }

    /// Every change is a whole-field assignment, so a poisoned lock still holds a consistent
    /// state.
    fn state(&self) -> MutexGuard<'_, State> {
        self.0.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn update(&self, change: impl FnOnce(&mut State)) {
        let mut state = self.state();
        change(&mut state);
        let Some(active) = &state.active else {
            // Nested sessions have no render GPU choice.
            return;
        };
        let device = |node: &Path| {
            let known = state.devices.get(node);
            json!({"node": node, "path": known.map_or(node, |d| &d.path),
                   "name": known.map_or("", |d| &d.name)})
        };
        // Report a configured path by the stable identity of the device it names.
        let identity = |path: &Option<PathBuf>| {
            path.as_ref().map(|path| {
                let node = fs::canonicalize(path).ok();
                state
                    .devices
                    .iter()
                    .find(|(known, _)| Some(*known) == node.as_ref())
                    .map_or_else(|| path.clone(), |(_, d)| d.path.clone())
            })
        };
        let configured = identity(&state.configured);
        let startup = identity(&state.startup);
        let file = state
            .config
            .as_ref()
            .map(|config| config.with_file_name(FILE));
        let (preference, detail) = &state.preference;
        let gpu = json!({
            "active": device(active),
            "startup": startup,
            "fallback": state.fallback,
            "configured": configured,
            "pending": configured != startup,
            "devices": state.devices.keys().map(|node| device(node)).collect::<Vec<_>>(),
            "outputs": state.outputs,
            "preference": {
                "state": preference, "detail": detail, "config": state.config, "file": file,
                "include": format!("include optional=true \"{FILE}\""),
            },
        });
        state.control.set_gpu(gpu);
    }

    /// Saves the next-login render GPU. Runs on the control service thread.
    fn save(&self, request: &str) -> Result<(), String> {
        // Serialize saves; each one compares, replaces and validates the managed file.
        static SAVING: Mutex<()> = Mutex::new(());
        let _saving = SAVING.lock().unwrap_or_else(PoisonError::into_inner);
        let (config, target) = {
            let state = self.state();
            if state.active.is_none() {
                return Err("Choosing the render GPU needs a native niri session".to_owned());
            }
            let config = state
                .config
                .clone()
                .ok_or("niri is not using a configuration file")?;
            let target = if request.is_empty() {
                None
            } else {
                let device = state
                    .devices
                    .values()
                    .find(|device| device.path == Path::new(request))
                    .ok_or_else(|| format!("Unknown render GPU: {request}"))?;
                Some(device.path.clone())
            };
            (config, target)
        };
        let file = config.with_file_name(FILE);
        let loaded = Config::load(&config);
        if loaded.config.is_err() {
            return Err(format!(
                "{} currently has errors; fix it before choosing the render GPU",
                config.display()
            ));
        }
        if !loaded.includes.iter().any(|path| same_file(path, &file)) {
            return Err(format!(
                "{} include optional=true \"{FILE}\"",
                adoption(&config)
            ));
        }
        let previous = read_managed(&file)?;
        let text = managed(target.as_deref())?;
        if previous.as_deref() != Some(text.as_str()) {
            replace(&file, previous.as_deref(), Some(&text))?;
        }

        // Validate the configuration the next login will read.
        let result = Config::load(&config);
        let effective = result
            .config
            .as_ref()
            .map(|config| config.debug.render_drm_device.clone());
        if effective.as_ref().ok() != Some(&target) {
            let reason = match (effective, &target) {
                (Err(_), _) => format!("{} did not load with the saved GPU", config.display()),
                (Ok(Some(other)), None) => format!(
                    "Another niri configuration file sets render-drm-device \"{}\"; remove it to \
                     select the GPU automatically",
                    other.display()
                ),
                (Ok(_), Some(_)) => format!(
                    "Another niri configuration file sets render-drm-device after the WaylandShader \
                     include; move the include to the end of {}",
                    config.display()
                ),
                (Ok(None), None) => unreachable!("automatic selection matched"),
            };
            return Err(match replace(&file, Some(&text), previous.as_deref()) {
                Ok(()) => reason,
                Err(err) => format!("{reason}. Restoring the previous file failed: {err}"),
            });
        }
        let preference = preference(Some(&config), &result.includes);
        self.update(|state| {
            state.configured = target;
            state.preference = preference;
        });
        Ok(())
    }
}

/// Adoption guidance; the controller shows the include line itself.
fn adoption(config: &Path) -> String {
    format!(
        "To choose the render GPU here, add this line at the end of {}:",
        config.display()
    )
}

/// Controller state for the managed file of `config`: whether a save can succeed.
fn preference(config: Option<&Path>, includes: &[PathBuf]) -> (&'static str, String) {
    let Some(config) = config else {
        return (
            "unavailable",
            "niri is not using a configuration file".to_owned(),
        );
    };
    let file = config.with_file_name(FILE);
    if !includes.iter().any(|path| same_file(path, &file)) {
        return ("not-adopted", adoption(config));
    }
    let directory = file.parent().unwrap_or(Path::new("/"));
    if let Err(err) = access(directory, Access::WRITE_OK) {
        return (
            "read-only",
            format!("{} is not writable: {err}", directory.display()),
        );
    }
    match read_managed(&file) {
        Ok(_) => ("ready", String::new()),
        Err(err) => ("read-only", err),
    }
}

/// Compares include paths, resolving symlinked directories but not the file itself.
fn same_file(a: &Path, b: &Path) -> bool {
    let canonical = |path: &Path| {
        path.parent()
            .and_then(|parent| fs::canonicalize(parent).ok())
            .zip(path.file_name())
            .map(|(parent, name)| parent.join(name))
    };
    a.components().eq(b.components()) || canonical(a).is_some_and(|a| Some(a) == canonical(b))
}

/// The `/dev/dri/by-path` link naming the same device as `node`.
fn stable_path(node: &Path) -> Option<PathBuf> {
    let node = fs::canonicalize(node).ok()?;
    fs::read_dir("/dev/dri/by-path")
        .ok()?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .find(|path| {
            path.to_str().is_some_and(|path| path.ends_with("-render"))
                && fs::canonicalize(path).ok().as_ref() == Some(&node)
        })
}

/// The exact managed file for a device, or automatic selection.
fn managed(device: Option<&Path>) -> Result<String, String> {
    let Some(device) = device else {
        return Ok(HEADER.to_owned());
    };
    let device = device
        .to_str()
        .filter(|path| !path.contains(['"', '\\']) && !path.contains(char::is_control))
        .ok_or_else(|| format!("Cannot store the device path {}", device.display()))?;
    Ok(format!(
        "{HEADER}debug {{\n    render-drm-device \"{device}\"\n}}\n"
    ))
}

/// Reads the managed file, refusing anything this controller did not write.
fn read_managed(file: &Path) -> Result<Option<String>, String> {
    let metadata = match fs::symlink_metadata(file) {
        Ok(metadata) => metadata,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(format!("Cannot inspect {}: {err}", file.display())),
    };
    if metadata.file_type().is_symlink() {
        return Err(format!(
            "{} is a symbolic link; change it where it is managed",
            file.display()
        ));
    }
    if !metadata.is_file() || metadata.permissions().readonly() {
        return Err(format!("{} is not a writable regular file", file.display()));
    }
    let text =
        fs::read_to_string(file).map_err(|err| format!("Cannot read {}: {err}", file.display()))?;
    let device = text
        .strip_prefix(HEADER)
        .and_then(|rest| rest.strip_prefix("debug {\n    render-drm-device \""))
        .and_then(|rest| rest.strip_suffix("\"\n}\n"));
    let ours = text == HEADER
        || device.is_some_and(|device| managed(Some(Path::new(device))).as_ref() == Ok(&text));
    if !ours {
        return Err(format!(
            "{} was edited outside the WaylandShader controller; edit or delete it manually",
            file.display()
        ));
    }
    Ok(Some(text))
}

/// Atomically replaces the managed file if it still has the `expected` content.
fn replace(file: &Path, expected: Option<&str>, text: Option<&str>) -> Result<(), String> {
    if read_managed(file)?.as_deref() != expected {
        return Err(format!(
            "{} changed while saving; try again",
            file.display()
        ));
    }
    let directory = file.parent().unwrap_or(Path::new("/"));
    let failed = |err: io::Error| format!("Cannot write {}: {err}", file.display());
    let Some(text) = text else {
        return match fs::remove_file(file) {
            Err(err) if err.kind() != io::ErrorKind::NotFound => Err(failed(err)),
            _ => Ok(()),
        };
    };
    let temporary = directory.join(format!(".{FILE}.{}.tmp", std::process::id()));
    let result = (|| {
        let mut new = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o644)
            .open(&temporary)?;
        new.write_all(text.as_bytes())?;
        new.sync_all()?;
        fs::rename(&temporary, file)?;
        File::open(directory)?.sync_all()
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result.map_err(failed)
}

#[cfg(test)]
mod tests {
    use super::*;

    const DEVICE: &str = "/dev/dri/by-path/pci-0000:03:00.0-render";
    const OTHER: &str = "debug {\n    render-drm-device \"/dev/dri/renderD129\"\n}\n";

    #[test]
    fn save_validates_precedence_and_never_overwrites_foreign_files() {
        let root = std::env::temp_dir().join(format!("waylandshader-gpu-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let config = root.join("config.kdl");
        let file = root.join(FILE);
        let include = format!("include optional=true \"{FILE}\"\n");
        let effective = || {
            Config::load(&config)
                .config
                .unwrap()
                .debug
                .render_drm_device
        };
        let gpu = Gpu::new(&Control::new());
        {
            let mut state = gpu.state();
            state.active = Some("/dev/dri/renderD129".into());
            let device = Device {
                path: DEVICE.into(),
                name: "test".into(),
            };
            state.devices.insert("/dev/dri/renderD128".into(), device);
        }
        gpu.config_loaded(&ConfigPath::Explicit(config.clone()), &[], Some(None));

        // Only an explicitly adopted file is written.
        fs::write(&config, "").unwrap();
        assert!(gpu.save(DEVICE).unwrap_err().contains("add this line"));
        assert!(!file.exists());
        assert!(gpu
            .save("/dev/dri/renderD128")
            .unwrap_err()
            .contains("Unknown"));

        fs::write(&config, &include).unwrap();
        gpu.save(DEVICE).unwrap();
        assert_eq!(effective().as_deref(), Some(Path::new(DEVICE)));
        gpu.save("").unwrap();
        assert_eq!(effective(), None);

        // A later setting wins over the include: the save is rolled back.
        fs::write(&config, format!("{include}{OTHER}")).unwrap();
        assert!(gpu.save(DEVICE).unwrap_err().contains("move the include"));
        assert_eq!(fs::read_to_string(&file).unwrap(), managed(None).unwrap());

        // An automatic file cannot clear an earlier explicit device.
        fs::write(&config, format!("{OTHER}{include}")).unwrap();
        gpu.save(DEVICE).unwrap();
        assert!(gpu.save("").unwrap_err().contains("remove it"));
        assert_eq!(effective().as_deref(), Some(Path::new(DEVICE)));

        // Hand-edited, read-only and symlinked files stay untouched.
        let edited = format!("{}// mine\n", managed(Some(Path::new(DEVICE))).unwrap());
        fs::write(&file, &edited).unwrap();
        assert!(gpu.save("").unwrap_err().contains("edited outside"));
        assert_eq!(fs::read_to_string(&file).unwrap(), edited);
        fs::write(&file, managed(None).unwrap()).unwrap();
        let mut permissions = fs::metadata(&file).unwrap().permissions();
        permissions.set_readonly(true);
        fs::set_permissions(&file, permissions).unwrap();
        assert!(gpu.save(DEVICE).is_err());
        fs::remove_file(&file).unwrap();
        std::os::unix::fs::symlink(root.join("elsewhere.kdl"), &file).unwrap();
        assert!(gpu.save(DEVICE).unwrap_err().contains("symbolic link"));
        assert!(fs::symlink_metadata(&file)
            .unwrap()
            .file_type()
            .is_symlink());

        fs::remove_dir_all(root).unwrap();
    }
}
