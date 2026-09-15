//! Desired settings and the isolated WaylandShader control service.
//!
//! The compositor only takes short in-memory locks. Filesystem operations, JSON
//! serialization and D-Bus delivery run on the control worker, never while holding
//! the settings lock. A bounded notification channel coalesces worker wakeups.

use std::collections::BTreeMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{mpsc, Arc, OnceLock};

use calloop::ping::{make_ping, Ping, PingSource};
use parking_lot::Mutex;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

const MAX_RECENT_PRESETS: usize = 10;

#[derive(Clone)]
pub struct Settings {
    pub generation: u64,
    pub preset_generation: u64,
    pub enabled: bool,
    pub preset: String,
    pub parameters: BTreeMap<String, f32>,
    pub outputs: BTreeMap<String, OutputSettings>,
}

#[derive(Clone, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase", deny_unknown_fields)]
pub struct OutputSettings {
    pub shader_enabled: bool,
    pub color_enabled: bool,
    pub gamma: f32,
    pub saturation: f32,
}

impl Default for OutputSettings {
    fn default() -> Self {
        Self {
            shader_enabled: true,
            color_enabled: false,
            gamma: 1.0,
            saturation: 1.0,
        }
    }
}

#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Config {
    enabled: bool,
    preset: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    recent_presets: Vec<String>,
    parameters: BTreeMap<String, f32>,
    outputs: BTreeMap<String, OutputSettings>,
}

struct Data {
    desired: Settings,
    // Never copy an uncompiled requested preset into this configuration.
    committed: Config,
    persist_revision: u64,
    pending_preset: bool,
    status: Value,
    config_error: String,
    service_error: String,
}

struct Inner {
    data: Mutex<Data>,
    config_path: Option<PathBuf>,
    redraw: OnceLock<Ping>,
    notify: OnceLock<mpsc::SyncSender<()>>,
}

#[derive(Clone)]
pub struct Control(Arc<Inner>);

impl Control {
    pub fn new() -> Self {
        let (config_path, config, config_error) = match config_path() {
            Ok(path) => {
                let (config, error) = match read_config(&path) {
                    Ok(config) => (config, String::new()),
                    Err(err) => (
                        Config::default(),
                        format!("Cannot read {}: {err}", path.display()),
                    ),
                };
                (Some(path), config, error)
            }
            Err(error) => (None, Config::default(), error),
        };
        let desired = Settings {
            generation: 0,
            preset_generation: 0,
            enabled: config.enabled,
            preset: config.preset.clone(),
            parameters: config.parameters.clone(),
            outputs: config.outputs.clone(),
        };
        let status = json!({
            "enabled": config.enabled, "active": false, "loading": false,
            "preset": "", "requestedPreset": "", "error": "",
            "parameters": [], "outputs": [],
        });
        Self(Arc::new(Inner {
            data: Mutex::new(Data {
                desired,
                committed: config,
                persist_revision: 0,
                pending_preset: false,
                status,
                config_error,
                service_error: String::new(),
            }),
            config_path,
            redraw: OnceLock::new(),
            notify: OnceLock::new(),
        }))
    }

    /// Starts the isolated control worker after the host registers its redraw source.
    ///
    /// The host callback must arrange a compositor redraw when the source fires.
    /// Startup errors are published through control status; repeated starts are ignored.
    pub fn start(&self, register_redraw: impl FnOnce(PingSource) -> Result<(), String>) {
        if self.0.notify.get().is_some() {
            return;
        }
        let (ping, source) = match make_ping() {
            Ok(pair) => pair,
            Err(err) => {
                self.service_error(format!("Cannot create WaylandShader redraw source: {err}"));
                return;
            }
        };
        if let Err(err) = register_redraw(source) {
            self.service_error(format!("Cannot attach WaylandShader redraw source: {err}"));
            return;
        }
        let _ = self.0.redraw.set(ping);
        let (sender, receiver) = mpsc::sync_channel(1);
        let _ = self.0.notify.set(sender);
        let worker_control = self.clone();
        if let Err(err) = std::thread::Builder::new()
            .name("waylandshader-control".to_owned())
            .spawn(move || run_worker(worker_control, receiver))
        {
            self.service_error(format!("Cannot start WaylandShader control worker: {err}"));
            return;
        }
        self.notify();
        self.wake();
    }

    pub fn snapshot_since(&self, generation: u64) -> Option<Settings> {
        let data = self.0.data.lock();
        (data.desired.generation != generation).then(|| data.desired.clone())
    }

    pub fn publish(&self, status: Value) {
        let mut data = self.0.data.lock();
        // The renderer tags publications so a frame using an older snapshot cannot
        // complete a newer request or overwrite a just-accepted mutation.
        if status
            .get("generation")
            .and_then(Value::as_u64)
            .is_some_and(|generation| generation != data.desired.generation)
            || status
                .get("presetGeneration")
                .and_then(Value::as_u64)
                .is_some_and(|generation| generation != data.desired.preset_generation)
        {
            return;
        }
        if data.status == status {
            return;
        }
        let loading = status["loading"].as_bool().unwrap_or(false);
        let preset = status["preset"].as_str().unwrap_or("");
        let error = status["error"].as_str().unwrap_or("");
        // Presentation/output errors are independent of the compile transaction.
        let preset_failed = status["presetFailed"]
            .as_bool()
            .unwrap_or(!error.is_empty());
        let mut rolled_back = false;
        if !loading && !preset_failed && preset == data.desired.preset {
            let parameters = data.desired.parameters.clone();
            if data.committed.preset != preset || data.committed.parameters != parameters {
                data.committed.preset = preset.to_owned();
                data.committed.parameters = parameters;
                data.persist_revision = data.persist_revision.wrapping_add(1);
            }
            if !preset.is_empty()
                && data.committed.recent_presets.first().map(String::as_str) != Some(preset)
            {
                data.committed.recent_presets.retain(|path| path != preset);
                data.committed.recent_presets.insert(0, preset.to_owned());
                data.committed.recent_presets.truncate(MAX_RECENT_PRESETS);
                data.persist_revision = data.persist_revision.wrapping_add(1);
            }
            data.pending_preset = false;
        } else if !loading && preset_failed && data.pending_preset {
            data.pending_preset = false;
            data.desired.preset = data.committed.preset.clone();
            data.desired.parameters = data.committed.parameters.clone();
            data.desired.generation = data.desired.generation.wrapping_add(1);
            rolled_back = true;
        }
        data.status = status;
        drop(data);
        self.notify();
        if rolled_back {
            self.wake();
        }
    }

    pub fn wake(&self) {
        if let Some(ping) = self.0.redraw.get() {
            ping.ping();
        }
    }

    fn notify(&self) {
        if let Some(sender) = self.0.notify.get() {
            // Full means a wakeup is already queued; the worker reads latest state.
            let _ = sender.try_send(());
        }
    }

    #[cfg(feature = "dbus")]
    fn status_value(&self) -> Value {
        let data = self.0.data.lock();
        let mut status = data.status.clone();
        status["recentPresets"] = json!(&data.committed.recent_presets);
        let errors: Vec<&str> = [
            status["error"].as_str().unwrap_or(""),
            &data.config_error,
            &data.service_error,
        ]
        .into_iter()
        .filter(|error| !error.is_empty())
        .collect();
        let error = errors.join("\n");
        status["error"] = Value::String(error);
        status
    }

    fn service_error(&self, error: String) {
        tracing::warn!("WaylandShader: {error}");
        self.0.data.lock().service_error = error;
        self.notify();
    }

    #[cfg(feature = "dbus")]
    fn mutate(
        &self,
        change: impl FnOnce(&mut Data) -> Result<(), ControlError>,
    ) -> Result<bool, ControlError> {
        let mut data = self.0.data.lock();
        change(&mut data)?;
        data.desired.generation = data.desired.generation.wrapping_add(1);
        // A successful mutation reply must already expose the accepted settings.
        // Keep active flags conservative until the renderer publishes a frame.
        // generation remains the last rendered generation so publish() still
        // observes the acknowledgement and commits parameter persistence.
        let Data {
            desired, status, ..
        } = &mut *data;
        status["enabled"] = Value::Bool(desired.enabled);
        if let Some(outputs) = status["outputs"].as_array_mut() {
            for output in outputs {
                if let Some(settings) = output["id"].as_str().and_then(|id| desired.outputs.get(id))
                {
                    output["shaderEnabled"] = Value::Bool(settings.shader_enabled);
                    output["colorEnabled"] = Value::Bool(settings.color_enabled);
                    output["gamma"] = Value::from(settings.gamma);
                    output["saturation"] = Value::from(settings.saturation);
                }
                output["shaderActive"] = Value::Bool(
                    desired.enabled
                        && output["shaderEnabled"].as_bool().unwrap_or(false)
                        && output["shaderActive"].as_bool().unwrap_or(false),
                );
                output["colorActive"] = Value::Bool(
                    desired.enabled
                        && output["colorEnabled"].as_bool().unwrap_or(false)
                        && output["colorActive"].as_bool().unwrap_or(false),
                );
            }
        }
        status["active"] = Value::Bool(status["outputs"].as_array().is_some_and(|outputs| {
            outputs.iter().any(|output| {
                output["shaderActive"].as_bool().unwrap_or(false)
                    || output["colorActive"].as_bool().unwrap_or(false)
            })
        }));
        if let Some(parameters) = status["parameters"].as_array_mut() {
            for parameter in parameters {
                if let Some(value) = parameter["name"]
                    .as_str()
                    .and_then(|name| desired.parameters.get(name))
                {
                    parameter["value"] = Value::from(*value);
                }
            }
        }
        drop(data);
        self.wake();
        self.notify();
        Ok(true)
    }

    #[cfg(feature = "dbus")]
    fn change_output(
        &self,
        id: &str,
        change: impl FnOnce(&mut OutputSettings),
    ) -> Result<bool, ControlError> {
        self.mutate(|data| {
            let observed = data.status["outputs"]
                .as_array()
                .and_then(|outputs| {
                    outputs
                        .iter()
                        .find(|output| output["id"].as_str() == Some(id))
                })
                .ok_or_else(|| {
                    ControlError::InvalidArgument(format!("Unknown connected output ID: {id}"))
                })?;
            let initial = OutputSettings {
                shader_enabled: observed["shaderEnabled"].as_bool().unwrap_or(true),
                color_enabled: observed["colorEnabled"].as_bool().unwrap_or(false),
                gamma: observed["gamma"].as_f64().unwrap_or(1.0) as f32,
                saturation: observed["saturation"].as_f64().unwrap_or(1.0) as f32,
            };
            let settings = data.desired.outputs.entry(id.to_owned()).or_insert(initial);
            change(settings);
            data.committed
                .outputs
                .insert(id.to_owned(), settings.clone());
            data.persist_revision = data.persist_revision.wrapping_add(1);
            Ok(())
        })
    }
}

fn config_path() -> Result<PathBuf, String> {
    if let Some(path) = std::env::var_os("WAYLANDSHADER_CONFIG") {
        let path = PathBuf::from(path);
        return if path.is_absolute() {
            Ok(path)
        } else {
            Err("WAYLANDSHADER_CONFIG must be an absolute file path".to_owned())
        };
    }
    let root = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|path| Path::new(path).is_absolute())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .ok_or_else(|| {
            "Neither XDG_CONFIG_HOME nor HOME provides a configuration directory".to_owned()
        })?;
    Ok(root.join("waylandshader/niri.json"))
}

fn read_config(path: &Path) -> Result<Config, String> {
    let bytes = match fs::read(path) {
        Ok(bytes) => bytes,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(Config::default()),
        Err(err) => return Err(err.to_string()),
    };
    let mut config: Config = serde_json::from_slice(&bytes).map_err(|err| err.to_string())?;
    if !config.preset.is_empty() && !valid_preset_path(&config.preset) {
        return Err("preset must be an absolute .slangp path or empty".to_owned());
    }
    if config
        .parameters
        .iter()
        .any(|(name, value)| name.is_empty() || name.contains('\0') || !value.is_finite())
    {
        return Err("parameter names must be nonempty and values finite".to_owned());
    }
    if config.outputs.iter().any(|(id, output)| {
        id.is_empty()
            || id.contains('\0')
            || !output.gamma.is_finite()
            || !(0.1..=5.0).contains(&output.gamma)
            || !output.saturation.is_finite()
            || !(0.0..=2.0).contains(&output.saturation)
    }) {
        return Err("invalid output ID, gamma (0.1..5), or saturation (0..2)".to_owned());
    }
    // History is advisory: stale files remain selectable, but malformed entries
    // must not prevent otherwise valid shader/output settings from loading.
    let mut recent = Vec::with_capacity(config.recent_presets.len().min(MAX_RECENT_PRESETS));
    for preset in config.recent_presets {
        if valid_preset_path(&preset) && !recent.contains(&preset) {
            recent.push(preset);
            if recent.len() == MAX_RECENT_PRESETS {
                break;
            }
        }
    }
    config.recent_presets = recent;
    Ok(config)
}

fn valid_preset_path(path: &str) -> bool {
    !path.contains('\0')
        && Path::new(path).is_absolute()
        && Path::new(path)
            .extension()
            .and_then(|ext| ext.to_str())
            .is_some_and(|ext| ext.eq_ignore_ascii_case("slangp"))
}

fn persist(path: &Path, config: &Config) -> Result<(), String> {
    static NEXT_TEMP: AtomicU64 = AtomicU64::new(0);
    let parent = path
        .parent()
        .ok_or_else(|| "configuration path has no parent".to_owned())?;
    fs::create_dir_all(parent).map_err(|err| err.to_string())?;
    let suffix = NEXT_TEMP.fetch_add(1, Ordering::Relaxed);
    let temporary = parent.join(format!(".niri.json.{}.{}.tmp", std::process::id(), suffix));
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&temporary)
        .map_err(|err| err.to_string())?;
    let result = (|| -> Result<(), String> {
        serde_json::to_writer_pretty(&mut file, config).map_err(|err| err.to_string())?;
        file.write_all(b"\n").map_err(|err| err.to_string())?;
        file.sync_all().map_err(|err| err.to_string())?;
        fs::rename(&temporary, path).map_err(|err| err.to_string())?;
        File::open(parent)
            .and_then(|directory| directory.sync_all())
            .map_err(|err| err.to_string())
    })();
    if result.is_err() {
        let _ = fs::remove_file(temporary);
    }
    result
}

#[cfg(feature = "dbus")]
#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.waylandshader.Error")]
enum ControlError {
    InvalidArgument(String),
    Busy(String),
    Failed(String),
}

#[cfg(feature = "dbus")]
struct Service(Control);

#[cfg(feature = "dbus")]
#[zbus::interface(name = "org.waylandshader.Effect")]
impl Service {
    #[zbus(name = "status")]
    fn status(&self) -> String {
        self.0.status_value().to_string()
    }

    #[zbus(name = "loadPreset")]
    fn load_preset(&self, path: String) -> Result<bool, ControlError> {
        // This method runs on zbus's worker, not on the compositor event loop.
        let path = if path.is_empty() {
            path
        } else {
            if !valid_preset_path(&path) {
                return Err(ControlError::InvalidArgument("Select a readable .slangp preset using an absolute path, or an empty path to unload.".to_owned()));
            }
            let file = File::open(&path).map_err(|err| {
                ControlError::InvalidArgument(format!("Cannot read preset: {err}"))
            })?;
            if !file
                .metadata()
                .map_err(|err| ControlError::InvalidArgument(err.to_string()))?
                .is_file()
            {
                return Err(ControlError::InvalidArgument(
                    "Preset is not a regular file".to_owned(),
                ));
            }
            fs::canonicalize(&path)
                .map_err(|err| ControlError::InvalidArgument(err.to_string()))?
                .into_os_string()
                .into_string()
                .map_err(|_| {
                    ControlError::InvalidArgument("Preset path must be UTF-8".to_owned())
                })?
        };
        self.0.mutate(|data| {
            if data.pending_preset || data.status["loading"].as_bool().unwrap_or(false) {
                return Err(ControlError::Busy(
                    "A shader preset is already compiling".to_owned(),
                ));
            }
            data.desired.parameters = if path == data.committed.preset {
                data.committed.parameters.clone()
            } else {
                BTreeMap::new()
            };
            data.desired.preset = path.clone();
            data.desired.preset_generation = data.desired.preset_generation.wrapping_add(1);
            data.pending_preset = true;
            data.status["loading"] = Value::Bool(true);
            data.status["requestedPreset"] = Value::String(path);
            data.status["error"] = Value::String(String::new());
            Ok(())
        })
    }

    #[zbus(name = "setEnabled")]
    fn set_enabled(&self, enabled: bool) -> Result<bool, ControlError> {
        self.0.mutate(|data| {
            data.desired.enabled = enabled;
            data.committed.enabled = enabled;
            data.persist_revision = data.persist_revision.wrapping_add(1);
            Ok(())
        })
    }

    #[zbus(name = "setParameter")]
    fn set_parameter(&self, name: String, value: f64) -> Result<bool, ControlError> {
        self.0.mutate(|data| {
            if data.pending_preset || data.status["loading"].as_bool().unwrap_or(false) {
                return Err(ControlError::Busy(
                    "Wait for the shader preset to finish compiling".to_owned(),
                ));
            }
            let parameter = data.status["parameters"]
                .as_array()
                .and_then(|parameters| {
                    parameters
                        .iter()
                        .find(|parameter| parameter["name"].as_str() == Some(&name))
                })
                .ok_or_else(|| {
                    ControlError::InvalidArgument(format!("Unknown shader parameter: {name}"))
                })?;
            let minimum = parameter["minimum"]
                .as_f64()
                .ok_or_else(|| ControlError::Failed("Missing parameter bounds".to_owned()))?;
            let maximum = parameter["maximum"]
                .as_f64()
                .ok_or_else(|| ControlError::Failed("Missing parameter bounds".to_owned()))?;
            let shader_value = value as f32;
            if !value.is_finite()
                || !shader_value.is_finite()
                || value < minimum
                || value > maximum
                || (shader_value as f64) < minimum
                || (shader_value as f64) > maximum
            {
                return Err(ControlError::InvalidArgument(format!(
                    "Parameter {name} must be finite and within {minimum}..{maximum}"
                )));
            }
            data.desired.parameters.insert(name, shader_value);
            Ok(())
        })
    }

    #[zbus(name = "setOutputShaderEnabled")]
    fn set_output_shader_enabled(&self, id: String, enabled: bool) -> Result<bool, ControlError> {
        self.0
            .change_output(&id, |output| output.shader_enabled = enabled)
    }

    #[zbus(name = "setOutputColorEnabled")]
    fn set_output_color_enabled(&self, id: String, enabled: bool) -> Result<bool, ControlError> {
        self.0
            .change_output(&id, |output| output.color_enabled = enabled)
    }

    #[zbus(name = "setOutputGamma")]
    fn set_output_gamma(&self, id: String, gamma: f64) -> Result<bool, ControlError> {
        if !gamma.is_finite() || !(0.1..=5.0).contains(&gamma) {
            return Err(ControlError::InvalidArgument(
                "Gamma must be finite and between 0.1 and 5".to_owned(),
            ));
        }
        self.0
            .change_output(&id, |output| output.gamma = gamma as f32)
    }

    #[zbus(name = "setOutputSaturation")]
    fn set_output_saturation(&self, id: String, saturation: f64) -> Result<bool, ControlError> {
        if !saturation.is_finite() || !(0.0..=2.0).contains(&saturation) {
            return Err(ControlError::InvalidArgument(
                "Saturation must be finite and between 0 and 2".to_owned(),
            ));
        }
        self.0
            .change_output(&id, |output| output.saturation = saturation as f32)
    }

    #[zbus(signal, name = "statusChanged")]
    async fn status_changed(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        status: &str,
    ) -> zbus::Result<()>;
}

#[cfg(feature = "dbus")]
fn connect(control: Control) -> zbus::Result<zbus::blocking::Connection> {
    let name = std::env::var("WAYLANDSHADER_DBUS_SERVICE")
        .unwrap_or_else(|_| "org.waylandshader.Niri".to_owned());
    let connection = zbus::blocking::Connection::session()?;
    connection
        .object_server()
        .at("/WaylandShader", Service(control))?;
    // Never queue for, replace, or acquire any stock niri/KDE service name.
    connection.request_name_with_flags(name, zbus::fdo::RequestNameFlags::DoNotQueue.into())?;
    Ok(connection)
}

fn run_worker(control: Control, receiver: mpsc::Receiver<()>) {
    #[cfg(feature = "dbus")]
    let connection = match connect(control.clone()) {
        Ok(connection) => Some(connection),
        Err(err) => {
            control.service_error(format!(
                "Cannot register WaylandShader D-Bus service: {err}"
            ));
            None
        }
    };
    #[cfg(not(feature = "dbus"))]
    control.service_error("This niri build has no D-Bus support".to_owned());

    let mut persisted_revision = 0;
    #[cfg(feature = "dbus")]
    let mut last_status = String::new();
    while receiver.recv().is_ok() {
        let pending = {
            let data = control.0.data.lock();
            (data.persist_revision != persisted_revision)
                .then(|| (data.persist_revision, data.committed.clone()))
        };
        if let Some((revision, config)) = pending {
            let result = control
                .0
                .config_path
                .as_deref()
                .ok_or_else(|| "No valid configuration path".to_owned())
                .and_then(|path| persist(path, &config));
            let error = result
                .err()
                .map(|err| format!("Cannot save niri shader settings: {err}"))
                .unwrap_or_default();
            if !error.is_empty() {
                tracing::warn!("WaylandShader: {error}");
            }
            control.0.data.lock().config_error = error;
            // Failure is visible and the next mutation retries, without an I/O loop.
            persisted_revision = revision;
        }
        #[cfg(feature = "dbus")]
        if let Some(connection) = &connection {
            let status = control.status_value().to_string();
            if status != last_status {
                if let Err(err) = connection.emit_signal(
                    None::<&str>,
                    "/WaylandShader",
                    "org.waylandshader.Effect",
                    "statusChanged",
                    &(status.as_str(),),
                ) {
                    let error = format!("Cannot deliver WaylandShader status: {err}");
                    tracing::warn!("WaylandShader: {error}");
                    control.0.data.lock().service_error = error;
                }
                last_status = status;
            }
        }
    }
}
