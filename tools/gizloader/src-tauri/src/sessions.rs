//! One long-lived SDK process owns each port. All UI operations and decoded
//! logs pass through this actor; no monitor process competes for its handle.
use crate::{logs, native, AppState, Job, Operation};
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    path::PathBuf,
    process::Stdio,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};
use tauri::ipc::Channel;
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    process::Command,
    sync::{mpsc, oneshot, watch},
    time::Instant,
};
use tokio_util::sync::CancellationToken;

struct Request {
    job: Job,
    progress: Channel<Value>,
    reply: oneshot::Sender<Result<Value, String>>,
}
struct Handle {
    id: String,
    sender: mpsc::Sender<Request>,
    cancel: CancellationToken,
    connected: Arc<AtomicBool>,
    status: Arc<Mutex<Option<Value>>>,
    stopped: watch::Receiver<bool>,
}
#[derive(Default)]
pub struct Sessions {
    ports: Mutex<HashMap<String, Handle>>,
    pub workers: AtomicUsize,
}
impl Sessions {
    pub fn active(&self, port: &str) -> bool {
        self.ports
            .lock()
            .unwrap()
            .get(port)
            .is_some_and(|h| !h.cancel.is_cancelled())
    }
    pub fn states(&self) -> Vec<Value> {
        self.ports.lock().unwrap().iter().map(|(id,h)| json!({ "id": id, "active": !h.cancel.is_cancelled(), "connected": h.connected.load(Ordering::SeqCst) && !h.cancel.is_cancelled(), "status": h.status.lock().unwrap().clone() })).collect()
    }
    pub fn snapshot(&self, port: &str) -> (bool, Option<Value>) {
        self.ports
            .lock()
            .unwrap()
            .get(port)
            .map(|h| {
                (
                    h.connected.load(Ordering::SeqCst) && !h.cancel.is_cancelled(),
                    h.status.lock().unwrap().clone(),
                )
            })
            .unwrap_or((false, None))
    }
    pub fn begin_disconnect(&self, port: &str) -> Option<watch::Receiver<bool>> {
        self.ports.lock().unwrap().get(port).map(|h| {
            h.connected.store(false, Ordering::SeqCst);
            h.cancel.cancel();
            h.stopped.clone()
        })
    }
    pub async fn wait_disconnected(stopped: Option<watch::Receiver<bool>>) {
        if let Some(mut stopped) = stopped {
            while !*stopped.borrow() {
                if stopped.changed().await.is_err() {
                    break;
                }
            }
        }
    }
    pub fn cancel_all(&self) {
        for handle in self.ports.lock().unwrap().values() {
            handle.cancel.cancel();
        }
    }
    pub fn reconcile(&self, ports: &[crate::Port]) {
        for (id, handle) in self.ports.lock().unwrap().iter() {
            if !ports.iter().any(|port| port.id == *id && port.present) {
                handle.cancel.cancel();
            }
        }
    }
}
struct Pending {
    request: Request,
    id: u64,
    status: Option<Value>,
    deadline: Instant,
    cancellation: Option<String>,
    acknowledged: u64,
}

impl Pending {
    fn record_progress(&mut self, acknowledged: u64, now: Instant) {
        // Stage has a stall deadline, not a fixed duration for the whole file.
        // Repeated/unchanged reports cannot keep a stuck transfer alive.
        if self.request.job.operation == Operation::Stage
            && self.cancellation.is_none()
            && acknowledged > self.acknowledged
        {
            self.deadline = now + Duration::from_secs(360);
            self.acknowledged = acknowledged;
        }
    }
}

pub fn binary() -> Result<PathBuf, String> {
    let extension = if cfg!(windows) { ".exe" } else { "" };
    let bundled = std::env::current_exe()
        .map_err(|e| e.to_string())?
        .with_file_name(format!("gizloader-session{extension}"));
    if bundled.is_file() {
        return Ok(bundled);
    }
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("binaries")
        .join(format!(
            "gizloader-session-{}{extension}",
            env!("NATIVE_TARGET")
        ));
    use sha2::{Digest, Sha256};
    let manifest: Value = serde_json::from_str(include_str!("../binaries/manifest.json"))
        .map_err(|e| e.to_string())?;
    let bytes = std::fs::read(&path).map_err(|e| e.to_string())?;
    if manifest["session"]["protocol"] != 1
        || manifest["session"]["sha256"].as_str() != Some(&format!("{:x}", Sha256::digest(bytes)))
    {
        return Err("Loader 会话程序校验失败。".into());
    }
    Ok(path)
}

pub async fn run(state: &AppState, job: &Job, progress: &Channel<Value>) -> Result<Value, String> {
    if job.cancel.is_cancelled() || state.0.closing.load(Ordering::SeqCst) {
        return Err("任务已取消。".into());
    }
    let sender = {
        let mut ports = state.0.sessions.ports.lock().unwrap();
        if job.operation != Operation::Connect
            && !ports
                .get(&job.port)
                .is_some_and(|h| h.connected.load(Ordering::SeqCst) && !h.cancel.is_cancelled())
        {
            return Err("请先连接串口。".into());
        }
        if let Some(handle) = ports.get(&job.port) {
            if handle.cancel.is_cancelled() {
                return Err("串口连接正在释放，请稍后重试。".into());
            }
            handle.sender.clone()
        } else {
            let (sender, receiver) = mpsc::channel(1);
            let cancel = CancellationToken::new();
            let id = uuid::Uuid::new_v4().to_string();
            let connected = Arc::new(AtomicBool::new(false));
            let status = Arc::new(Mutex::new(None));
            let (stopped, stopped_rx) = watch::channel(false);
            ports.insert(
                job.port.clone(),
                Handle {
                    id: id.clone(),
                    sender: sender.clone(),
                    cancel: cancel.clone(),
                    connected: connected.clone(),
                    status: status.clone(),
                    stopped: stopped_rx,
                },
            );
            state.0.sessions.workers.fetch_add(1, Ordering::SeqCst);
            let state = state.clone();
            let port = job.port.clone();
            tauri::async_runtime::spawn(async move {
                actor(&state, &port, receiver, &cancel, &connected, &status).await;
                connected.store(false, Ordering::SeqCst);
                let mut ports = state.0.sessions.ports.lock().unwrap();
                if ports.get(&port).is_some_and(|handle| handle.id == id) {
                    ports.remove(&port);
                }
                state.0.sessions.workers.fetch_sub(1, Ordering::SeqCst);
                stopped.send_replace(true);
            });
            sender
        }
    };
    let (reply, response) = oneshot::channel();
    sender
        .send(Request {
            job: job.clone(),
            progress: progress.clone(),
            reply,
        })
        .await
        .map_err(|_| "Loader 连接已关闭。".to_string())?;
    response
        .await
        .map_err(|_| "Loader 会话意外退出。".to_string())?
}

fn command(id: u64, job: &Job) -> String {
    let operation = match job.operation {
        Operation::Connect | Operation::Status => "status",
        Operation::Stage => "stage",
        Operation::RebootApp => "rebootApp",
        Operation::RebootLoader => "rebootLoader",
        Operation::RebootUpgrade => "rebootUpgrade",
    };
    if let Some(package) = job
        .package
        .as_ref()
        .filter(|_| job.operation == Operation::Stage)
    {
        let path = package.file.path().to_string_lossy();
        let encoded: String = path.bytes().map(|byte| format!("{byte:02x}")).collect();
        format!("{id} {operation} {encoded}\n")
    } else {
        format!("{id} {operation}\n")
    }
}
fn decode_hex(text: &str) -> Result<Vec<u8>, String> {
    if text.len() > 1024 || !text.len().is_multiple_of(2) || !text.is_ascii() {
        return Err("无效的日志帧。".into());
    }
    text.as_bytes()
        .as_chunks::<2>()
        .0
        .iter()
        .map(|chunk| {
            let value = |b: u8| match b {
                b'0'..=b'9' => Some(b - b'0'),
                b'a'..=b'f' => Some(b - b'a' + 10),
                _ => None,
            };
            Ok(value(chunk[0]).ok_or("日志帧编码无效。")? * 16
                + value(chunk[1]).ok_or("日志帧编码无效。")?)
        })
        .collect()
}

async fn actor(
    state: &AppState,
    port: &str,
    mut requests: mpsc::Receiver<Request>,
    cancel: &CancellationToken,
    connected: &AtomicBool,
    status_cache: &Mutex<Option<Value>>,
) {
    let mut process = Command::new(&state.0.session_binary);
    process
        .args(["--port", port])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    #[cfg(windows)]
    process.creation_flags(0x08000000);
    let mut child = match process.spawn() {
        Ok(child) => child,
        Err(error) => {
            requests.close();
            while let Some(request) = requests.recv().await {
                let _ = request
                    .reply
                    .send(Err(format!("Loader 会话启动失败：{error}")));
            }
            return;
        }
    };
    let mut stdin = child.stdin.take().unwrap();
    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let mut out = [0; 4096];
    let mut err = [0; 4096];
    let mut frames = native::Lines::default();
    let mut diagnostics = native::Lines::default();
    let mut streams: HashMap<String, native::LogLines> = HashMap::new();
    let mut pending: Option<Pending> = None;
    let mut sequence = 0;
    let mut qualified = false;
    let mut stderr_done = false;
    let mut failure = "Loader 连接已关闭。".to_string();
    let mut ticker = tokio::time::interval(Duration::from_millis(50));
    'session: loop {
        tokio::select! {
            _ = cancel.cancelled() => break,
            request = requests.recv(), if pending.is_none() => {
                let Some(request) = request else { break; };
                sequence += 1;
                let phase = if connected.load(Ordering::SeqCst) { "precheck" } else { "connect" };
                let _ = request.progress.send(json!({"phase":phase,"progress":0}));
                let frame = command(sequence, &request.job);
                pending = Some(Pending { request, id: sequence, status: None, deadline: Instant::now() + Duration::from_secs(360), cancellation: None, acknowledged: 0 });
                if let Err(error) = stdin.write_all(frame.as_bytes()).await { failure = error.to_string(); break; }
            }
            _ = ticker.tick(), if pending.is_some() => {
                let current = pending.as_mut().unwrap();
                if current.cancellation.is_none() && (current.request.job.cancel.is_cancelled() || current.request.reply.is_closed() || Instant::now() >= current.deadline) {
                    let reason = if Instant::now() >= current.deadline { "操作超时。" } else { "任务已取消。" };
                    current.cancellation = Some(reason.into()); current.deadline = Instant::now() + Duration::from_secs(5);
                    if stdin.write_all(format!("{} cancel\n", current.id).as_bytes()).await.is_err() { break; }
                } else if current.cancellation.is_some() && Instant::now() >= current.deadline {
                    failure = current.cancellation.clone().unwrap(); break;
                }
            }
            read = stdout.read(&mut out) => {
                let count = match read { Ok(0) => break, Ok(n) => n, Err(error) => { failure = error.to_string(); break; } };
                for frame in frames.push(&out[..count], false) {
                    let event: Value = match serde_json::from_str(&frame) { Ok(value) => value, Err(_) => { failure = "Loader 会话返回了无效帧。".into(); break 'session; } };
                    match event["event"].as_str() {
                        Some("status") => {
                            let value = event["value"].clone();
                            if !logs::valid_status(&value) { failure = "Loader 返回的设备状态无效。".into(); break 'session; }
                            if !qualified { streams.clear(); }
                            qualified = true; connected.store(true, Ordering::SeqCst);
                            *status_cache.lock().unwrap() = Some(value.clone());
                            state.0.logs.connected(port, &value);
                            if let Some(current) = pending.as_mut().filter(|current| event["id"].as_u64() == Some(current.id)) { current.status = Some(value); }
                        }
                        Some("disconnected") => { connected.store(false, Ordering::SeqCst); streams.clear(); state.0.logs.disconnected(port); },
                        Some("data") => {
                            let source = event["source"].as_str().unwrap_or_default();
                            if !matches!(source, "device" | "stdout") { failure = "无效的日志来源。".into(); break 'session; }
                            let bytes = match decode_hex(event["hex"].as_str().unwrap_or_default()) { Ok(bytes) => bytes, Err(error) => { failure = error; break 'session; } };
                            if connected.load(Ordering::SeqCst) {
                                let (lines, rejected) = streams.entry(source.into()).or_default().push(&bytes, false);
                                for line in lines { state.0.logs.push(port, source, &line); }
                                if rejected > 0 { state.0.logs.push(port, "system", &format!("Loader 诊断：已忽略 {rejected} 段非文本输出")); }
                            }
                        }
                        Some("progress") => {
                            if let Some(current) = pending.as_mut().filter(|current| event["id"].as_u64() == Some(current.id)) {
                                let acked = event["acked"].as_u64().unwrap_or(0); let total = event["total"].as_u64().unwrap_or(0);
                                if total == 0 || acked > total { failure = "无效的传输进度。".into(); break 'session; }
                                current.record_progress(acked, Instant::now());
                                let _ = current.request.progress.send(json!({"phase":"stage","progress":acked as f64 * 100.0 / total as f64,"acknowledgedBytes":acked,"totalBytes":total}));
                            }
                        }
                        Some("done") => {
                            let Some(current) = pending.take() else { failure = "意外的任务结果。".into(); break 'session; };
                            if event["id"].as_u64() != Some(current.id) { let _ = current.request.reply.send(Err("Loader 任务编号不匹配。".into())); break 'session; }
                            let result = if let Some(reason) = current.cancellation { Err(reason) }
                                else if event["code"].as_i64() == Some(0) { current.status.ok_or_else(|| "Loader 未返回最终状态。".into()) }
                                else { Err(format!("Loader 操作失败（code={}）。", event["code"])) };
                            if qualified { state.0.logs.push(port, "system", &match &result { Ok(_) => format!("任务完成：{:?}", current.request.job.operation), Err(error) => error.clone() }); }
                            let _ = current.request.reply.send(result);
                            if !qualified { break 'session; }
                        }
                        _ => { failure = "未知的 Loader 会话消息。".into(); break 'session; }
                    }
                }
            }
            read = stderr.read(&mut err), if !stderr_done => {
                match read {
                    Ok(count) => { stderr_done = count == 0; for line in diagnostics.push(&err[..count], stderr_done) { failure = line.clone(); if qualified { state.0.logs.push(port, "stderr", &line); } } }
                    Err(_) => stderr_done = true,
                }
            }
        }
    }
    requests.close();
    drop(stdin);
    // EOF cooperatively stops the adapter; kill only after the bounded grace.
    if tokio::time::timeout(Duration::from_secs(3), child.wait())
        .await
        .is_err()
    {
        let _ = child.kill().await;
    }
    for (source, mut lines) in streams {
        for line in lines.push(&[], true).0 {
            if qualified {
                state.0.logs.push(port, &source, &line);
            }
        }
    }
    state.0.logs.disconnected(port);
    if let Some(current) = pending {
        let _ = current.request.reply.send(Err(failure.clone()));
    }
    while let Some(request) = requests.recv().await {
        let _ = request.reply.send(Err(failure.clone()));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn sdk_log_text_survives_multibyte_splits_in_bridge_callbacks() {
        let text = format!("{}中文日志\n", "x".repeat(511));
        let mut lines = native::LogLines::default();
        let mut output = Vec::new();
        for bytes in text.as_bytes().chunks(512) {
            let frame: String = bytes.iter().map(|byte| format!("{byte:02x}")).collect();
            let (decoded, rejected) = lines.push(&decode_hex(&frame).unwrap(), false);
            assert_eq!(rejected, 0);
            output.extend(decoded);
        }
        assert_eq!(output, vec![text.trim_end()]);
        assert!(!output[0].contains('�'));
    }

    #[test]
    fn advancing_stage_can_finish_after_six_minutes_but_duplicates_cannot_extend_a_stall() {
        let (reply, _) = oneshot::channel();
        let job = Job {
            port: "COM1".into(),
            operation: Operation::Stage,
            package: None,
            cancel: CancellationToken::new(),
            started: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false)),
        };
        let start = Instant::now();
        let mut pending = Pending {
            request: Request {
                job,
                progress: Channel::new(|_| Ok(())),
                reply,
            },
            id: 1,
            status: None,
            deadline: start + Duration::from_secs(360),
            cancellation: None,
            acknowledged: 0,
        };
        pending.record_progress(100, start + Duration::from_secs(300));
        pending.record_progress(200, start + Duration::from_secs(600));
        assert!(pending.deadline > start + Duration::from_secs(900));
        let deadline = pending.deadline;
        pending.record_progress(200, start + Duration::from_secs(950));
        assert_eq!(pending.deadline, deadline);
        pending.cancellation = Some("cancelled".into());
        pending.record_progress(300, start + Duration::from_secs(1000));
        assert_eq!(pending.deadline, deadline);
    }

    #[test]
    fn device_bytes_cannot_escape_hex_framing() {
        assert_eq!(
            decode_hex("7b226576656e74223a22646f6e65227d0a").unwrap(),
            b"{\"event\":\"done\"}\n"
        );
        assert!(decode_hex("zz").is_err());
        assert!(decode_hex("中").is_err());
        assert!(decode_hex(&"aa".repeat(513)).is_err());
    }
    #[cfg(unix)]
    #[tokio::test]
    async fn requests_reuse_one_process_and_rescan_keeps_it_alive() {
        use std::os::unix::fs::PermissionsExt;
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("fixture");
        std::fs::write(&path, "#!/bin/sh\nwhile read id operation rest; do\n printf '{\"event\":\"status\",\"id\":%s,\"value\":{\"deviceUid\":\"001122334455\",\"board\":\"fixture\",\"active\":{\"role\":\"loader\"},\"pid\":%s}}\\n' \"$id\" \"$$\"\n printf '{\"event\":\"data\",\"source\":\"device\",\"hex\":\"68656c6c6f0a\"}\\n'\n printf '{\"event\":\"done\",\"id\":%s,\"code\":0}\\n' \"$id\"\ndone\n").unwrap();
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
        let mut state = AppState::new(path.clone());
        std::sync::Arc::get_mut(&mut state.0)
            .unwrap()
            .session_binary = path;
        let mut job = Job {
            port: "COM1".into(),
            operation: Operation::Connect,
            package: None,
            cancel: CancellationToken::new(),
            started: std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false)),
        };
        let progress = Channel::new(|_| Ok(()));
        job.operation = Operation::Status;
        assert_eq!(
            run(&state, &job, &progress).await.unwrap_err(),
            "请先连接串口。"
        );
        assert_eq!(state.0.sessions.workers.load(Ordering::SeqCst), 0);
        job.operation = Operation::Connect;
        let first = run(&state, &job, &progress).await.unwrap();
        assert!(state.0.sessions.snapshot("COM1").0);
        job.operation = Operation::Status;
        state.0.sessions.reconcile(&[crate::Port {
            id: "COM1".into(),
            name: "fixture".into(),
            usb_vendor_id: None,
            usb_product_id: None,
            usb_serial_number: None,
            selected_by_default: true,
            initial_status: None,
            initial_error: None,
            present: true,
            remembered: false,
            connected: true,
            active: true,
        }]);
        let second = run(&state, &job, &progress).await.unwrap();
        assert_eq!(first["pid"], second["pid"]);
        assert_eq!(state.0.sessions.workers.load(Ordering::SeqCst), 1);
        let pid = first["pid"].as_i64().unwrap() as i32;
        let stopped = state.0.sessions.begin_disconnect("COM1");
        assert!(!state.0.sessions.active("COM1"));
        assert!(!state.0.sessions.snapshot("COM1").0);
        assert!(run(&state, &job, &progress).await.is_err());
        job.operation = Operation::Connect;
        assert!(run(&state, &job, &progress).await.is_err());
        tokio::time::timeout(Duration::from_secs(5), Sessions::wait_disconnected(stopped))
            .await
            .unwrap();
        assert_eq!(unsafe { libc::kill(pid, 0) }, -1);
        assert!(state.0.sessions.ports.lock().unwrap().is_empty());
        job.operation = Operation::Stage;
        assert_eq!(
            run(&state, &job, &progress).await.unwrap_err(),
            "请先连接串口。"
        );
        job.operation = Operation::Connect;
        let reconnected = run(&state, &job, &progress).await.unwrap();
        assert_ne!(first["pid"], reconnected["pid"]);
        let pid = reconnected["pid"].as_i64().unwrap() as i32;
        state.cancel_all();
        tokio::time::timeout(Duration::from_secs(5), async {
            while state.0.sessions.workers.load(Ordering::SeqCst) > 0 {
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        })
        .await
        .unwrap();
        assert_eq!(unsafe { libc::kill(pid, 0) }, -1);
        assert!(state.0.sessions.ports.lock().unwrap().is_empty());
    }
}
