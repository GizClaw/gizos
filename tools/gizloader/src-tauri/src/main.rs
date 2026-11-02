#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod cli;
mod download;
mod forge;
mod logs;
mod native;
mod package;
mod ports;
mod sessions;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::{HashMap, HashSet},
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};
use tauri::{ipc::Channel, Manager, State, WindowEvent};
use tokio_util::sync::CancellationToken;

#[derive(Clone, Copy, Debug, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
enum Operation {
    Connect,
    Status,
    Stage,
    RebootApp,
    RebootLoader,
    RebootUpgrade,
}

#[derive(Clone)]
struct Job {
    port: String,
    operation: Operation,
    package: Option<Arc<package::Package>>,
    cancel: CancellationToken,
    started: Arc<AtomicBool>,
}

struct Inner {
    binary: PathBuf,
    session_binary: PathBuf,
    sessions: sessions::Sessions,
    logs: logs::Logs,
    ports: Mutex<HashSet<String>>,
    preferences: ports::Preferences,
    packages: Mutex<HashMap<String, Arc<package::Package>>>,
    jobs: Mutex<HashMap<String, Job>>,
    picker: Mutex<Option<(String, CancellationToken)>>,
    closing: AtomicBool,
    scanning: AtomicBool,
}

#[derive(Clone)]
struct AppState(Arc<Inner>);
impl AppState {
    fn new(binary: PathBuf) -> Self {
        Self(Arc::new(Inner {
            session_binary: binary.with_file_name(if cfg!(windows) {
                "gizloader-session.exe"
            } else {
                "gizloader-session"
            }),
            binary,
            sessions: sessions::Sessions::default(),
            logs: logs::Logs::default(),
            ports: Mutex::new(HashSet::new()),
            preferences: ports::Preferences::default(),
            packages: Mutex::new(HashMap::new()),
            jobs: Mutex::new(HashMap::new()),
            picker: Mutex::new(None),
            closing: AtomicBool::new(false),
            scanning: AtomicBool::new(false),
        }))
    }
    fn cancel_all(&self) {
        self.0.sessions.cancel_all();
        if let Some((_, cancel)) = self.0.picker.lock().unwrap().as_ref() {
            cancel.cancel();
        }
        for job in self.0.jobs.lock().unwrap().values() {
            job.cancel.cancel();
        }
        self.0.packages.lock().unwrap().clear();
    }
    fn prepare(
        &self,
        port: String,
        operation: Operation,
        package_id: Option<String>,
    ) -> Result<String, String> {
        if self.0.closing.load(Ordering::SeqCst) {
            return Err("应用正在关闭。".into());
        }
        if !self.0.ports.lock().unwrap().contains(&port) {
            return Err("串口已变化，请重新扫描。".into());
        }
        let package = if operation == Operation::Stage {
            Some(
                self.0
                    .packages
                    .lock()
                    .unwrap()
                    .get(&package_id.ok_or("请先选择固件包。")?)
                    .cloned()
                    .ok_or("固件包不可用，请重新选择。")?,
            )
        } else {
            None
        };
        let mut jobs = self.0.jobs.lock().unwrap();
        if self.0.closing.load(Ordering::SeqCst)
            || self.0.scanning.load(Ordering::SeqCst)
            || !self.0.ports.lock().unwrap().contains(&port)
        {
            return Err("串口正在扫描或关闭，请稍后重试。".into());
        }
        if jobs.len() >= 16 {
            return Err("最多同时处理 16 台设备。".into());
        }
        if jobs.values().any(|job| job.port == port) {
            return Err("此设备已有任务。".into());
        }
        let id = uuid::Uuid::new_v4().to_string();
        jobs.insert(
            id.clone(),
            Job {
                port,
                operation,
                package,
                cancel: CancellationToken::new(),
                started: Arc::new(AtomicBool::new(false)),
            },
        );
        Ok(id)
    }
}

struct JobGuard {
    state: AppState,
    id: String,
}
impl Drop for JobGuard {
    fn drop(&mut self) {
        self.state.0.jobs.lock().unwrap().remove(&self.id);
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Port {
    id: String,
    name: String,
    usb_vendor_id: Option<u16>,
    usb_product_id: Option<u16>,
    usb_serial_number: Option<String>,
    selected_by_default: bool,
    initial_status: Option<Value>,
    initial_error: Option<String>,
    present: bool,
    remembered: bool,
    connected: bool,
    active: bool,
}

#[tauri::command]
fn native_info(state: State<'_, AppState>) -> Value {
    let _ = &state.0.binary;
    let manifest: Value = serde_json::from_str(include_str!("../binaries/manifest.json"))
        .expect("Build-generated native manifest");
    json!({ "transport": "native", "commit": manifest["commit"] })
}

struct ScanGuard(AppState);
impl Drop for ScanGuard {
    fn drop(&mut self) {
        self.0 .0.scanning.store(false, Ordering::SeqCst);
    }
}

fn enumerate_ports() -> Result<Vec<Port>, String> {
    let discovered = serialport::available_ports().map_err(|e| e.to_string())?;
    let ports: Vec<Port> = discovered
        .into_iter()
        // Darwin exposes callout and dial-in aliases for each physical port.
        // Keep the callout endpoint, matching the native Host Serial provider.
        .filter(|port| !cfg!(target_os = "macos") || port.port_name.starts_with("/dev/cu."))
        .map(|port| {
            let (name, vid, pid, serial, selected) = match port.port_type {
                serialport::SerialPortType::UsbPort(info) => (
                    info.product
                        .or(info.manufacturer)
                        .unwrap_or_else(|| "USB Serial".into()),
                    Some(info.vid),
                    Some(info.pid),
                    info.serial_number,
                    true,
                ),
                _ => ("Serial".into(), None, None, None, false),
            };
            Port {
                id: port.port_name,
                name,
                usb_vendor_id: vid,
                usb_product_id: pid,
                usb_serial_number: serial,
                selected_by_default: selected,
                initial_status: None,
                initial_error: None,
                present: true,
                remembered: false,
                connected: false,
                active: false,
            }
        })
        .collect();
    Ok(ports)
}

async fn list_ports(state: &AppState, add_new: bool) -> Result<Vec<Port>, String> {
    let _scan = {
        let jobs = state.0.jobs.lock().unwrap();
        if state.0.closing.load(Ordering::SeqCst) || !jobs.is_empty() {
            return Err("请等待当前任务结束后再扫描。".into());
        }
        if state.0.scanning.swap(true, Ordering::SeqCst) {
            return Err("串口正在扫描。".into());
        }
        ScanGuard(state.clone())
    };
    let discovered = tauri::async_runtime::spawn_blocking(enumerate_ports)
        .await
        .map_err(|e| e.to_string())??;
    let mut ports = state.0.preferences.scan(discovered, add_new)?;
    *state.0.ports.lock().unwrap() = ports
        .iter()
        .filter(|p| p.present)
        .map(|p| p.id.clone())
        .collect();
    state.0.sessions.reconcile(&ports);
    state.0.logs.discover(&ports);
    for port in &mut ports {
        let (connected, status) = state.0.sessions.snapshot(&port.id);
        port.connected = connected;
        port.active = state.0.sessions.active(&port.id);
        port.initial_status = status;
    }
    Ok(ports)
}
#[tauri::command]
async fn scan_ports(state: State<'_, AppState>) -> Result<Vec<Port>, String> {
    list_ports(state.inner(), true).await
}
#[tauri::command]
async fn remembered_ports(state: State<'_, AppState>) -> Result<Vec<Port>, String> {
    list_ports(state.inner(), false).await
}
#[tauri::command]
fn connection_states(state: State<'_, AppState>) -> Vec<Value> {
    state.0.sessions.states()
}
#[tauri::command]
async fn disconnect_port(port_id: String, state: State<'_, AppState>) -> Result<(), String> {
    let stopped = {
        let jobs = state.0.jobs.lock().unwrap();
        if jobs.values().any(|j| j.port == port_id) || state.0.scanning.load(Ordering::SeqCst) {
            return Err("请先取消此串口的任务，或等待扫描结束。".into());
        }
        state.0.sessions.begin_disconnect(&port_id)
    };
    sessions::Sessions::wait_disconnected(stopped).await;
    Ok(())
}
#[tauri::command]
async fn forget_port(port_id: String, state: State<'_, AppState>) -> Result<(), String> {
    let stopped = {
        let jobs = state.0.jobs.lock().unwrap();
        if jobs.values().any(|j| j.port == port_id) || state.0.scanning.load(Ordering::SeqCst) {
            return Err("请先取消此串口的任务，或等待扫描结束。".into());
        }
        state.0.preferences.remove(&port_id)?;
        state.0.ports.lock().unwrap().remove(&port_id);
        state.0.sessions.begin_disconnect(&port_id)
    };
    sessions::Sessions::wait_disconnected(stopped).await;
    Ok(())
}

#[tauri::command]
async fn inspect_package(path: String, state: State<'_, AppState>) -> Result<Value, String> {
    let package = Arc::new(
        tauri::async_runtime::spawn_blocking(move || package::inspect(&PathBuf::from(path)))
            .await
            .map_err(|e| e.to_string())??,
    );
    if state.0.closing.load(Ordering::SeqCst) {
        return Err("固件选择已关闭。".into());
    }
    let id = uuid::Uuid::new_v4().to_string();
    let result = json!({ "id": id, "name": package.name, "size": package.size, "metadata": package.metadata });
    state.0.packages.lock().unwrap().insert(id, package);
    Ok(result)
}

#[tauri::command]
fn prepare_job(
    port_id: String,
    operation: Operation,
    package_id: Option<String>,
    state: State<'_, AppState>,
) -> Result<String, String> {
    state.prepare(port_id, operation, package_id)
}

#[tauri::command]
fn cancel_job(job_id: String, state: State<'_, AppState>) {
    if let Some(job) = state.0.jobs.lock().unwrap().get(&job_id) {
        job.cancel.cancel();
    }
}

#[tauri::command]
fn release_package(package_id: String, state: State<'_, AppState>) {
    state.0.packages.lock().unwrap().remove(&package_id);
}

#[tauri::command]
fn close_client(state: State<'_, AppState>) {
    state.cancel_all();
    state.0.packages.lock().unwrap().clear();
}

#[tauri::command]
fn prepare_forge_picker(state: State<'_, AppState>) -> Result<String, String> {
    if state.0.closing.load(Ordering::SeqCst) {
        return Err("应用正在关闭。".into());
    }
    let id = uuid::Uuid::new_v4().to_string();
    let mut picker = state.0.picker.lock().unwrap();
    if let Some((_, cancel)) = picker.take() {
        cancel.cancel();
    }
    *picker = Some((id.clone(), CancellationToken::new()));
    Ok(id)
}

#[tauri::command]
fn cancel_forge_picker(selection_id: String, state: State<'_, AppState>) {
    if let Some((id, cancel)) = state.0.picker.lock().unwrap().as_ref() {
        if *id == selection_id {
            cancel.cancel();
        }
    }
}

#[tauri::command]
async fn forge_package(
    app: tauri::AppHandle,
    selection_id: String,
    progress: Channel<Value>,
    state: State<'_, AppState>,
) -> Result<Option<Value>, String> {
    let cancel = state
        .0
        .picker
        .lock()
        .unwrap()
        .as_ref()
        .filter(|(id, _)| *id == selection_id)
        .map(|(_, cancel)| cancel.clone())
        .ok_or("固件选择已失效。")?;
    let result = forge::choose(&app, &cancel, &progress).await;
    let mut picker = state.0.picker.lock().unwrap();
    if picker.as_ref().is_some_and(|(id, _)| *id == selection_id) {
        *picker = None;
    }
    drop(picker);
    let Some(package) = result? else {
        return Ok(None);
    };
    if cancel.is_cancelled() || state.0.closing.load(Ordering::SeqCst) {
        return Err("固件选择已关闭。".into());
    }
    let package = Arc::new(package);
    let id = uuid::Uuid::new_v4().to_string();
    let result = json!({ "id": id, "name": package.name, "size": package.size, "metadata": package.metadata });
    state.0.packages.lock().unwrap().insert(id, package);
    Ok(Some(result))
}

#[tauri::command]
async fn download_package(
    url: String,
    selection_id: String,
    progress: Channel<Value>,
    state: State<'_, AppState>,
) -> Result<Value, String> {
    let cancel = state
        .0
        .picker
        .lock()
        .unwrap()
        .as_ref()
        .filter(|(id, _)| *id == selection_id)
        .map(|(_, cancel)| cancel.clone())
        .ok_or("固件选择已失效。")?;
    let result = download::package(
        download::Request {
            url,
            name: None,
            size: None,
            sha256: None,
            redirects: true,
        },
        &cancel,
        &progress,
    )
    .await;
    let mut picker = state.0.picker.lock().unwrap();
    if picker.as_ref().is_some_and(|(id, _)| *id == selection_id) {
        *picker = None;
    }
    drop(picker);
    let package = Arc::new(result?);
    if cancel.is_cancelled() || state.0.closing.load(Ordering::SeqCst) {
        return Err("固件选择已关闭。".into());
    }
    let id = uuid::Uuid::new_v4().to_string();
    let result = json!({ "id": id, "name": package.name, "size": package.size, "metadata": package.metadata });
    state.0.packages.lock().unwrap().insert(id, package);
    Ok(result)
}

#[tauri::command]
async fn execute_job(
    job_id: String,
    progress: Channel<Value>,
    state: State<'_, AppState>,
) -> Result<Value, String> {
    let state = state.inner().clone();
    let job = state
        .0
        .jobs
        .lock()
        .unwrap()
        .get(&job_id)
        .cloned()
        .ok_or("任务不存在。")?;
    if job.started.swap(true, Ordering::SeqCst) {
        return Err("任务已开始执行。".into());
    }
    let _guard = JobGuard {
        state: state.clone(),
        id: job_id,
    };
    sessions::run(&state, &job, &progress).await
}

fn cli_arguments(mut arguments: Vec<std::ffi::OsString>) -> Option<Vec<std::ffi::OsString>> {
    if arguments
        .first()
        .is_some_and(|argument| argument == "--gui")
    {
        return None;
    }
    if arguments
        .first()
        .is_some_and(|argument| argument == "--cli")
    {
        arguments.remove(0);
    } else if arguments.is_empty() {
        return None;
    }
    Some(arguments)
}

fn run_cli(arguments: Vec<std::ffi::OsString>) -> ! {
    let result = native::binary().and_then(|binary| cli::run(&binary, &arguments));
    match result {
        Ok(code) => std::process::exit(code),
        Err(error) => {
            eprintln!("Gizloader: {error}");
            std::process::exit(1);
        }
    }
}

fn main() {
    let arguments = std::env::args_os().skip(1).collect::<Vec<_>>();
    cli::dispatch_worker(&arguments);
    if let Some(arguments) = cli_arguments(arguments) {
        run_cli(arguments);
    }
    let app = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .on_window_event(|window, event| {
            if window.label() == "main" {
                if let WindowEvent::CloseRequested { api, .. } = event {
                    api.prevent_close();
                    window.app_handle().exit(0);
                }
            }
        })
        .setup(|app| {
            let binary = native::binary().map_err(std::io::Error::other)?;
            let session_binary = sessions::binary().map_err(std::io::Error::other)?;
            let mut state = AppState::new(binary);
            let history = app.path().app_config_dir()?.join("port-list.json");
            let inner = Arc::get_mut(&mut state.0).unwrap();
            inner.session_binary = session_binary;
            inner.preferences = ports::Preferences::open(history).map_err(std::io::Error::other)?;
            app.manage(state);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            native_info,
            scan_ports,
            remembered_ports,
            connection_states,
            disconnect_port,
            forget_port,
            inspect_package,
            release_package,
            prepare_job,
            execute_job,
            cancel_job,
            close_client,
            prepare_forge_picker,
            cancel_forge_picker,
            forge_package,
            download_package,
            logs::open_log_window,
            logs::read_logs,
            logs::clear_logs,
            logs::export_logs
        ])
        .build(tauri::generate_context!())
        .expect("Failed to start Gizloader");
    app.run(|app, event| {
        if let tauri::RunEvent::ExitRequested { api, .. } = event {
            let state = app.state::<AppState>().inner().clone();
            if !state.0.closing.swap(true, Ordering::SeqCst) {
                state.cancel_all();
                if state.0.jobs.lock().unwrap().is_empty()
                    && state.0.picker.lock().unwrap().is_none()
                    && state.0.sessions.workers.load(Ordering::SeqCst) == 0
                {
                    return;
                }
                api.prevent_exit();
                let app = app.clone();
                tauri::async_runtime::spawn(async move {
                    for _ in 0..40 {
                        if state.0.jobs.lock().unwrap().is_empty()
                            && state.0.picker.lock().unwrap().is_none()
                            && state.0.sessions.workers.load(Ordering::SeqCst) == 0
                        {
                            break;
                        }
                        tokio::time::sleep(Duration::from_millis(100)).await;
                    }
                    app.exit(0);
                });
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cli_dispatch_preserves_arguments_and_explicit_gui() {
        let args = |values: &[&str]| values.iter().map(std::ffi::OsString::from).collect();
        assert!(cli_arguments(vec![]).is_none());
        assert!(cli_arguments(args(&["--gui"])).is_none());
        assert_eq!(
            cli_arguments(args(&["--cli", "scan", "--no-ble"])),
            Some(args(&["scan", "--no-ble"]))
        );
        assert_eq!(cli_arguments(args(&["--help"])), Some(args(&["--help"])));
        assert_eq!(cli_arguments(args(&["--cli"])), Some(vec![]));
    }

    #[test]
    fn releasing_package_owner_keeps_the_active_job_snapshot() {
        let state = AppState::new(PathBuf::from("unused"));
        state.0.ports.lock().unwrap().insert("COM1".into());
        let file = tempfile::NamedTempFile::new().unwrap();
        let path = file.path().to_path_buf();
        state.0.packages.lock().unwrap().insert(
            "package".into(),
            Arc::new(package::Package {
                file,
                name: "app.tar.zlib".into(),
                size: 1,
                metadata: package::Metadata {
                    role: "app".into(),
                    board: "test".into(),
                    target: "esp32s3".into(),
                    version: "1.0.0".into(),
                },
            }),
        );
        let id = state
            .prepare("COM1".into(), Operation::Stage, Some("package".into()))
            .unwrap();
        state.0.packages.lock().unwrap().remove("package");
        assert!(path.exists());
        drop(JobGuard { state, id });
        assert!(!path.exists());
    }

    #[test]
    fn job_registry_enforces_port_ownership_limit_and_early_cancellation() {
        let state = AppState::new(PathBuf::from("unused"));
        *state.0.ports.lock().unwrap() = (0..17).map(|i| format!("COM{i}")).collect();
        assert!(state
            .prepare("COM99".into(), Operation::Status, None)
            .is_err());
        let id = state
            .prepare("COM0".into(), Operation::Status, None)
            .unwrap();
        assert!(state
            .prepare("COM0".into(), Operation::Status, None)
            .is_err());
        state.cancel_all();
        assert!(state.0.jobs.lock().unwrap()[&id].cancel.is_cancelled());
        for i in 1..16 {
            state
                .prepare(format!("COM{i}"), Operation::Status, None)
                .unwrap();
        }
        assert!(state
            .prepare("COM16".into(), Operation::Status, None)
            .is_err());
        drop(JobGuard {
            state: state.clone(),
            id,
        });
        assert!(state
            .prepare("COM16".into(), Operation::Status, None)
            .is_ok());
    }
}
