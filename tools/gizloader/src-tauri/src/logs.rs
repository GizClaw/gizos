use crate::AppState;
use serde::Serialize;
use std::{
    collections::{BTreeMap, VecDeque},
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};
use tauri::{Manager, State, WebviewUrl, WebviewWindowBuilder};
use tauri_plugin_dialog::DialogExt;

const MAX_RECORDS: usize = 20_000;
const MAX_BYTES: usize = 8 * 1024 * 1024;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Record {
    id: u64,
    timestamp: u64,
    port: String,
    source: String,
    message: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct PortView {
    id: String,
    name: String,
    board: String,
    present: bool,
    monitoring: bool,
    status: String,
}

#[derive(Default)]
struct Buffer {
    records: VecDeque<Record>,
    bytes: usize,
    sequence: u64,
    cleared_through: u64,
    dropped: usize,
    ports: BTreeMap<String, PortView>,
}

#[derive(Default)]
pub struct Logs {
    buffer: Mutex<Buffer>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    records: Vec<Record>,
    ports: Vec<PortView>,
    cursor: u64,
    oldest: u64,
    cleared_through: u64,
    dropped: usize,
    has_more: bool,
}

impl Logs {
    pub fn push(&self, port: &str, source: &str, text: &str) {
        let text = plain_text(text);
        let mut end = text.len().min(8192);
        while !text.is_char_boundary(end) {
            end -= 1;
        }
        let message = text[..end].to_string();
        let mut buffer = self.buffer.lock().unwrap();
        buffer.sequence += 1;
        let record = Record {
            id: buffer.sequence,
            timestamp: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_millis() as u64,
            port: port.into(),
            source: source.into(),
            message,
        };
        buffer.bytes += record.message.len() + record.port.len() + record.source.len();
        buffer.records.push_back(record);
        while buffer.records.len() > MAX_RECORDS || buffer.bytes > MAX_BYTES {
            let old = buffer.records.pop_front().unwrap();
            buffer.bytes -= old.message.len() + old.port.len() + old.source.len();
            buffer.dropped += 1;
        }
    }
    pub fn connected(&self, port: &str, status: &serde_json::Value) {
        let mut buffer = self.buffer.lock().unwrap();
        let board = status["board"].as_str().unwrap_or("Loader").to_string();
        let name = format!(
            "{board} · {}",
            status["active"]["role"].as_str().unwrap_or_default()
        );
        buffer.ports.insert(
            port.into(),
            PortView {
                id: port.into(),
                name,
                board,
                present: true,
                monitoring: true,
                status: "connected".into(),
            },
        );
    }
    pub fn disconnected(&self, port: &str) {
        if let Some(view) = self.buffer.lock().unwrap().ports.get_mut(port) {
            view.monitoring = false;
            view.status = "disconnected".into();
        }
    }
    pub fn discover(&self, ports: &[crate::Port]) {
        let mut buffer = self.buffer.lock().unwrap();
        let retained: std::collections::HashSet<_> =
            buffer.records.iter().map(|r| r.port.clone()).collect();
        buffer
            .ports
            .retain(|id, view| view.monitoring || retained.contains(id));
        for view in buffer.ports.values_mut() {
            view.present = ports.iter().any(|port| port.id == view.id);
        }
    }
    fn read(&self, after: u64) -> Snapshot {
        let buffer = self.buffer.lock().unwrap();
        let records: Vec<_> = buffer
            .records
            .iter()
            .filter(|r| r.id > after)
            .take(1000)
            .cloned()
            .collect();
        let cursor = records.last().map_or(buffer.sequence, |r| r.id);
        Snapshot {
            records,
            ports: buffer.ports.values().cloned().collect(),
            cursor,
            oldest: buffer.records.front().map_or(buffer.sequence + 1, |r| r.id),
            cleared_through: buffer.cleared_through,
            dropped: buffer.dropped,
            has_more: cursor < buffer.sequence,
        }
    }
    fn clear(&self) {
        let mut buffer = self.buffer.lock().unwrap();
        buffer.records.clear();
        buffer.bytes = 0;
        buffer.dropped = 0;
        buffer.cleared_through = buffer.sequence;
    }
}

#[tauri::command]
pub async fn open_log_window(app: tauri::AppHandle) -> Result<(), String> {
    if let Some(window) = app.get_webview_window("serial-logs") {
        window.unminimize().map_err(|e| e.to_string())?;
        window.show().map_err(|e| e.to_string())?;
        return window.set_focus().map_err(|e| e.to_string());
    }
    WebviewWindowBuilder::new(
        &app,
        "serial-logs",
        WebviewUrl::App("desktop/loader.html?view=logs".into()),
    )
    .title("串口日志 · Gizloader")
    .inner_size(1180.0, 760.0)
    .min_inner_size(800.0, 500.0)
    .center()
    .build()
    .map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
pub fn read_logs(after: u64, state: State<'_, AppState>) -> Snapshot {
    state.0.logs.read(after)
}
#[tauri::command]
pub fn clear_logs(state: State<'_, AppState>) {
    state.0.logs.clear();
}

pub fn valid_status(status: &serde_json::Value) -> bool {
    matches!(status["active"]["role"].as_str(), Some("app" | "loader"))
        && status["deviceUid"]
            .as_str()
            .is_some_and(|uid| uid.len() == 12 && uid.bytes().all(|b| b.is_ascii_hexdigit()))
}

#[tauri::command(async)]
pub fn export_logs(app: tauri::AppHandle, content: String) -> Result<Option<String>, String> {
    // Export the frozen viewer snapshot, including records evicted while paused.
    if content.len() > 16 * 1024 * 1024 {
        return Err("导出内容超过 16 MiB。".into());
    }
    let Some(path) = app
        .dialog()
        .file()
        .set_file_name("h6-serial-logs.txt")
        .add_filter("Text", &["txt"])
        .blocking_save_file()
    else {
        return Ok(None);
    };
    let path = path.into_path().map_err(|e| e.to_string())?;
    std::fs::write(&path, content).map_err(|e| e.to_string())?;
    Ok(Some(path.to_string_lossy().into()))
}

fn plain_text(text: &str) -> String {
    let mut chars = text.chars().peekable();
    let mut result = String::new();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            match chars.next() {
                Some('[') => {
                    for c in chars.by_ref() {
                        if ('@'..='~').contains(&c) {
                            break;
                        }
                    }
                }
                Some(']') => {
                    while let Some(c) = chars.next() {
                        if c == '\u{7}' || (c == '\u{1b}' && chars.peek() == Some(&'\\')) {
                            if c == '\u{1b}' {
                                chars.next();
                            }
                            break;
                        }
                    }
                }
                _ => {}
            }
        } else if !c.is_control() || c == '\t' {
            result.push(c);
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn device_escape_sequences_are_plain_text() {
        assert_eq!(plain_text("\u{1b}[31m错误\u{1b}[0m <script>\u{1b}]8;;https://example.com\u{7}link\u{1b}]8;;\u{1b}\\"), "错误 <script>link");
    }

    #[test]
    fn history_is_ordered_bounded_and_clear_never_reuses_a_cursor() {
        let logs = Logs::default();
        for i in 0..20_100 {
            logs.push(if i % 2 == 0 { "COM1" } else { "COM2" }, "device", "hello");
        }
        let first = logs.read(0);
        assert_eq!(first.oldest, 101);
        assert_eq!(first.dropped, 100);
        assert_eq!(first.records.len(), 1000);
        assert!(first.has_more);
        assert_eq!(logs.read(first.cursor).records[0].id, first.cursor + 1);
        logs.clear();
        logs.push("COM1", "device", "new");
        let fresh = logs.read(first.cursor);
        assert_eq!(fresh.cleared_through, 20_100);
        assert_eq!(fresh.records[0].id, 20_101);
        assert_eq!(fresh.dropped, 0);
    }
    #[test]
    fn byte_limit_and_multibyte_line_limit_are_enforced() {
        let logs = Logs::default();
        for _ in 0..1100 {
            logs.push("COM1", "device", &"中".repeat(5000));
        }
        let buffer = logs.buffer.lock().unwrap();
        assert!(buffer.bytes <= MAX_BYTES);
        assert!(buffer.dropped > 0);
        assert_eq!(buffer.records[0].message.len(), 8190);
    }
}
