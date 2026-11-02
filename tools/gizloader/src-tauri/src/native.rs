#[cfg(test)]
use serde_json::json;
use serde_json::Value;
use std::path::PathBuf;
#[cfg(test)]
use tokio::{io::AsyncReadExt, process::Command};
#[cfg(test)]
use tokio_util::sync::CancellationToken;

pub fn binary() -> Result<PathBuf, String> {
    let extension = if cfg!(windows) { ".exe" } else { "" };
    let bundled = std::env::current_exe()
        .map_err(|e| e.to_string())?
        .with_file_name(format!("gizloader-cli{extension}"));
    // macOS bundling signs this executable and changes its full-file digest.
    // The bundled copy is protected by the application's code signature.
    if bundled.is_file() {
        return Ok(bundled);
    }
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("binaries")
        .join(format!(
            "gizloader-cli-{}{extension}",
            env!("NATIVE_TARGET")
        ));
    use sha2::{Digest, Sha256};
    let manifest: Value = serde_json::from_str(include_str!("../binaries/manifest.json"))
        .map_err(|e| e.to_string())?;
    let bytes = std::fs::read(&path).map_err(|e| format!("原生 H2Loader 无法读取：{e}"))?;
    if manifest["sha256"].as_str() != Some(&format!("{:x}", Sha256::digest(bytes))) {
        return Err("原生 H2Loader 校验失败，请重新构建桌面应用。".into());
    }
    Ok(path)
}

#[cfg(test)]
fn fields<'a>(line: &'a str, marker: &str) -> Result<HashMap<&'a str, &'a str>, String> {
    let mut fields = HashMap::new();
    for field in line
        .strip_prefix(marker)
        .ok_or("无效的原生输出。")?
        .split_whitespace()
    {
        let (key, value) = field.split_once('=').ok_or("原生输出字段无效。")?;
        if fields.insert(key, value).is_some() {
            return Err("原生输出字段重复。".into());
        }
    }
    Ok(fields)
}

#[cfg(test)]
pub fn status(line: &str) -> Result<Value, String> {
    let fields = fields(line, "H2_LOADER_STATUS ")?;
    fn required<'a>(fields: &HashMap<&str, &'a str>, key: &str) -> Result<&'a str, String> {
        fields
            .get(key)
            .copied()
            .ok_or_else(|| format!("设备状态缺少 {key}。"))
    }
    let get = |key: &str| required(&fields, key);
    let capabilities = u32::from_str_radix(
        get("capabilities")?
            .strip_prefix("0x")
            .ok_or("无效的能力字段。")?,
        16,
    )
    .map_err(|e| e.to_string())?;
    let commands = u32::from_str_radix(
        get("command_availability")?
            .strip_prefix("0x")
            .ok_or("无效的命令字段。")?,
        16,
    )
    .map_err(|e| e.to_string())?;
    let uid = get("device_uid")?;
    if commands & !0x001f3f3f != 0
        || capabilities & !7 != 0
        || uid.len() != 12
        || !uid.bytes().all(|v| v.is_ascii_hexdigit())
        || !["app", "loader"].contains(&get("active_role")?)
    {
        return Err("设备状态无效。".into());
    }
    let number = |key| get(key)?.parse::<u32>().map_err(|error| error.to_string());
    let metadata = |prefix: &str| -> Result<Value, String> {
        let field = |suffix| {
            get(&format!("{prefix}_{suffix}")).map(|value| if value == "-" { "" } else { value })
        };
        Ok(json!({
            "valid": field("valid")? == "1",
            "packageChecksum": field("package_checksum")?, "packageSize": field("package_size")?,
            "imageChecksum": field("image_checksum")?, "imageSize": field("image_size")?,
            "role": field("role")?, "version": field("version")?, "board": field("board")?, "target": field("target")?,
        }))
    };
    Ok(json!({
        "board": get("board")?, "target": get("target")?, "chip": get("chip")?, "deviceUid": uid,
        "capabilities": capabilities, "commandAvailability": commands,
        "active": { "role": get("active_role")?, "version": get("active_version")?, "imageChecksum": get("active_checksum")?, "imageSize": get("active_image_size")? },
        "runningPartition": number("running_partition")?, "nextPartition": number("next_partition")?, "bootIntent": get("boot_intent")?,
        "stage": metadata("stage")?, "partition1": metadata("partition_1")?, "partition2": metadata("partition_2")?,
        "lastResult": get("last_result")?.parse::<i32>().map_err(|error| error.to_string())?,
        "mfg": { "mode": number("mfg_mode")?, "steps": get("mfg_steps")?.bytes().map(|v| v.checked_sub(b'0').filter(|v| *v <= 3).ok_or("无效的 MFG 状态。".to_string())).collect::<Result<Vec<_>, _>>()? },
    }))
}

#[cfg(test)]
pub fn progress(line: &str) -> Result<Option<Value>, String> {
    if !line.starts_with("H2_LOADER_SEND state=progress ") {
        return Ok(None);
    }
    let fields = fields(line, "H2_LOADER_SEND ")?;
    let bytes = |key| {
        fields
            .get(key)
            .ok_or("传输进度缺少字节计数。")?
            .parse::<u64>()
            .map_err(|e| e.to_string())
    };
    let acknowledged = bytes("acked")?;
    let total = bytes("total")?;
    if total == 0 || acknowledged > total {
        return Err("传输进度字节计数无效。".into());
    }
    let percent = acknowledged as f64 * 100.0 / total as f64;
    Ok(Some(
        json!({ "phase": "stage", "acknowledgedBytes": acknowledged, "totalBytes": total, "progress": percent, "percent": percent }),
    ))
}

#[cfg(test)]
async fn stop(child: &mut tokio::process::Child) {
    #[cfg(unix)]
    if let Some(pid) = child.id() {
        // The pinned CLI cooperatively cancels and closes its PAL connection.
        unsafe {
            libc::kill(pid as libc::pid_t, libc::SIGTERM);
        }
    }
    #[cfg(windows)]
    let _ = child.start_kill();
    if tokio::time::timeout(Duration::from_secs(3), child.wait())
        .await
        .is_err()
    {
        let _ = child.kill().await;
    }
}

#[cfg(test)]
pub async fn run(
    binary: &Path,
    args: &[String],
    cancel: &CancellationToken,
    on_progress: impl FnMut(Value),
) -> Result<Option<Value>, String> {
    run_stream(binary, args, cancel, false, on_progress, |_, _| {}).await
}

// Bounded framing also handles invalid UTF-8 and output without a newline.
#[derive(Default)]
pub(crate) struct Lines(Vec<u8>);
impl Lines {
    pub(crate) fn push(&mut self, bytes: &[u8], flush: bool) -> Vec<String> {
        self.0.extend_from_slice(bytes);
        let mut result = Vec::new();
        loop {
            let end = self.0.iter().position(|byte| *byte == b'\n').map(|i| i + 1);
            let count = match end {
                Some(end) if end <= 8192 => end,
                _ if self.0.len() >= 8192 => {
                    // Keep a split multibyte character for the next chunk.
                    let mut end = 8192;
                    while end > 8188 && self.0.get(end).is_some_and(|b| b & 0xc0 == 0x80) {
                        end -= 1;
                    }
                    end
                }
                _ if flush && !self.0.is_empty() => self.0.len(),
                _ => break,
            };
            result.push(
                String::from_utf8_lossy(&self.0.drain(..count).collect::<Vec<_>>())
                    .trim_end_matches(['\r', '\n'])
                    .to_string(),
            );
        }
        result
    }
}

#[cfg(test)]
pub async fn run_stream(
    binary: &Path,
    args: &[String],
    cancel: &CancellationToken,
    monitor: bool,
    mut on_progress: impl FnMut(Value),
    mut on_log: impl FnMut(&str, &str),
) -> Result<Option<Value>, String> {
    if cancel.is_cancelled() {
        return Err("任务已取消。".into());
    }
    let mut command = Command::new(binary);
    command
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    #[cfg(windows)]
    command.creation_flags(0x08000000); // CREATE_NO_WINDOW
    let mut child = command
        .spawn()
        .map_err(|e| format!("原生 H2Loader 启动失败：{e}"))?;
    let mut stdout = child.stdout.take().unwrap();
    let mut stderr = child.stderr.take().unwrap();
    let mut out_lines = Lines::default();
    let mut err_lines = Lines::default();
    let mut out_bytes = [0; 4096];
    let mut err_bytes = [0; 4096];
    let mut out_done = false;
    let mut err_done = false;
    let mut errors = String::new();
    let deadline = tokio::time::Instant::now() + Duration::from_secs(360);
    let mut result = None;
    let mut failure = None;
    while !out_done || !err_done {
        tokio::select! {
            _ = cancel.cancelled() => { failure = Some("任务已取消。".to_string()); break; }
            _ = tokio::time::sleep_until(deadline), if !monitor => { failure = Some("操作超时。".to_string()); break; }
            read = stdout.read(&mut out_bytes), if !out_done => match read {
                Ok(count) => {
                    out_done = count == 0;
                    for line in out_lines.push(&out_bytes[..count], out_done) {
                        on_log("stdout", &line);
                        if !monitor {
                            if line.starts_with("H2_LOADER_STATUS ") {
                                match status(&line) { Ok(value) => result = Some(value), Err(error) => failure = Some(error) }
                            }
                            match progress(&line) {
                                Ok(Some(value)) => on_progress(value),
                                Ok(None) => {},
                                Err(error) => failure = Some(error),
                            }
                        }
                    }
                }
                Err(error) => { failure = Some(error.to_string()); }
            },
            read = stderr.read(&mut err_bytes), if !err_done => match read {
                Ok(count) => {
                    err_done = count == 0;
                    for line in err_lines.push(&err_bytes[..count], err_done) {
                        on_log("stderr", &line);
                        errors.push_str(&line);
                        errors.push('\n');
                        if errors.len() > 8192 {
                            let mut start = errors.len() - 8192;
                            while !errors.is_char_boundary(start) { start += 1; }
                            errors.drain(..start);
                        }
                    }
                }
                Err(error) => { failure = Some(error.to_string()); }
            }
        }
        if failure.is_some() {
            break;
        }
    }
    if let Some(failure) = failure {
        stop(&mut child).await;
        for line in out_lines.push(&[], true) {
            on_log("stdout", &line);
        }
        for line in err_lines.push(&[], true) {
            on_log("stderr", &line);
        }
        return Err(failure);
    }
    let exit = tokio::select! {
        _ = cancel.cancelled() => { stop(&mut child).await; return Err("任务已取消。".into()); }
        _ = tokio::time::sleep_until(deadline), if !monitor => { stop(&mut child).await; return Err("操作超时。".into()); }
        result = child.wait() => result.map_err(|e| e.to_string())?,
    };
    if !exit.success() {
        return Err(if errors.is_empty() {
            format!("设备操作失败（{exit}）。")
        } else {
            errors.trim().to_string()
        });
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn output_framing_handles_split_utf8_crlf_and_unterminated_floods() {
        let mut lines = Lines::default();
        assert!(lines.push(&[0xe4, 0xb8], false).is_empty());
        assert_eq!(lines.push(&[0xad, b'\r', b'\n'], false), vec!["中"]);
        let chunks = lines.push(&vec![b'x'; 25_000], false);
        assert_eq!(chunks.len(), 3);
        assert!(chunks.iter().all(|line| line.len() == 8192));
        assert_eq!(lines.push(&[0xff], true)[0].len(), 427);
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn both_streams_are_delivered_and_stderr_failure_is_preserved() {
        let mut entries = Vec::new();
        let result = run_stream(
            Path::new("/bin/sh"),
            &[
                "-c".into(),
                "printf 'device one\\n'; printf 'diagnostic\\n' >&2; printf 'tail'; exit 7".into(),
            ],
            &CancellationToken::new(),
            true,
            |_| {},
            |source, text| entries.push((source.to_string(), text.to_string())),
        )
        .await;
        assert!(result.unwrap_err().contains("diagnostic"));
        assert!(entries.contains(&("stdout".into(), "device one".into())));
        assert!(entries.contains(&("stdout".into(), "tail".into())));
        assert!(entries.contains(&("stderr".into(), "diagnostic".into())));
    }

    #[test]
    fn acknowledged_progress_rejects_invalid_or_duplicate_counts() {
        let value = progress(
            "H2_LOADER_SEND state=progress acked=1024 total=4096 percent=25.0 rate_bps=123",
        )
        .unwrap()
        .unwrap();
        assert_eq!(value["progress"], 25.0);
        assert_eq!(value["acknowledgedBytes"], 1024);
        assert!(progress("H2_LOADER_SEND state=progress acked=9 total=8").is_err());
        assert!(progress("H2_LOADER_SEND state=progress acked=1 acked=2 total=8").is_err());
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn cancellation_reaps_the_owned_process() {
        let token = CancellationToken::new();
        let cancel = token.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(50)).await;
            cancel.cancel();
        });
        let result = run(
            Path::new("/bin/sh"),
            &[
                "-c".into(),
                "while :; do printf 'waiting\\n'; sleep 0.05; done".into(),
            ],
            &token,
            |_| {},
        )
        .await;
        assert!(result.unwrap_err().contains("取消"));
    }
}

#[cfg(test)]
use std::{collections::HashMap, path::Path, process::Stdio, time::Duration};

/// Text boundary for Loader SDK output. The SDK's console callback can also
/// forward non-frame boot/ROM bytes. Keep complete UTF-8 log text; never render
/// invalid bytes through replacement characters or join different sessions.
#[derive(Default)]
pub(crate) struct LogLines {
    pending: Vec<u8>,
    discarding: bool,
}
impl LogLines {
    pub(crate) fn push(&mut self, bytes: &[u8], flush: bool) -> (Vec<String>, usize) {
        let mut lines = Vec::new();
        let mut rejected = 0;
        for &byte in bytes {
            if byte == b'\n' {
                if !self.discarding {
                    match std::str::from_utf8(&self.pending) {
                        Ok(text) => lines.push(text.trim_end_matches('\r').into()),
                        Err(_) => rejected += 1,
                    }
                }
                self.pending.clear();
                self.discarding = false;
                continue;
            }
            if self.discarding {
                continue;
            }
            if byte.is_ascii_control() && !matches!(byte, b'\t' | b'\r' | 7 | 27) {
                self.pending.clear();
                self.discarding = true;
                rejected += 1;
                continue;
            }
            self.pending.push(byte);
            if self.pending.len() >= 8192 {
                let end = match std::str::from_utf8(&self.pending) {
                    Ok(_) => self.pending.len(),
                    Err(error) if error.error_len().is_none() => error.valid_up_to(),
                    Err(_) => {
                        self.pending.clear();
                        self.discarding = true;
                        rejected += 1;
                        continue;
                    }
                };
                // end is a UTF-8 boundary proven by the validator above.
                lines.push(std::str::from_utf8(&self.pending[..end]).unwrap().into());
                self.pending.drain(..end);
            }
        }
        if flush {
            if !self.pending.is_empty() && !self.discarding {
                match std::str::from_utf8(&self.pending) {
                    Ok(text) => lines.push(text.trim_end_matches('\r').into()),
                    Err(_) => rejected += 1,
                }
            }
            self.pending.clear();
            self.discarding = false;
        }
        (lines, rejected)
    }
}

#[cfg(test)]
mod log_text_tests {
    use super::LogLines;
    #[test]
    fn binary_prefix_and_boot_flood_never_become_garbled_log_text() {
        let mut stream = LogLines::default();
        let (lines, rejected) = stream.push(b"\xff\x80boot bytes\nI (10) loader: ready\n", false);
        assert_eq!(lines, ["I (10) loader: ready"]);
        assert_eq!(rejected, 1);
        let (lines, rejected) = stream.push(&vec![0xff; 30_000], false);
        assert!(lines.is_empty());
        assert_eq!(rejected, 1);
        let (lines, rejected) = stream.push(b"\nnormal log\n", false);
        assert_eq!(lines, ["normal log"]);
        assert_eq!(rejected, 0);
    }
    #[test]
    fn chinese_is_preserved_across_callbacks_and_long_line_boundaries() {
        let text = format!("{}中文日志\n", "a".repeat(8191));
        let mut stream = LogLines::default();
        let mut lines = Vec::new();
        for bytes in text.as_bytes().chunks(512) {
            let (next, rejected) = stream.push(bytes, false);
            assert_eq!(rejected, 0);
            lines.extend(next);
        }
        assert_eq!(lines.concat(), text.trim_end());
        let mut stream = LogLines::default();
        assert_eq!(stream.push(&[0xe4, 0xb8], false).0.len(), 0);
        assert_eq!(stream.push(&[0xad, b'\n'], false).0, ["中"]);
    }
}
