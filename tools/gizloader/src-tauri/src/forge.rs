use crate::{download, package};
use serde::Deserialize;
use serde_json::Value;
use std::{
    sync::{Arc, Mutex},
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tauri::{ipc::Channel, WebviewUrl, WebviewWindowBuilder, WindowEvent};
use tokio::sync::oneshot;
use tokio_util::sync::CancellationToken;
use url::Url;

const CALLBACK: &str = "https://www.h6infra.com/tools/forge-picker/callback";
const MAX_BYTES: u64 = download::MAX_BYTES;

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct FileDescriptor {
    pub name: String,
    pub download_url: String,
    pub size: u64,
    pub sha256: Option<String>,
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_millis() as u64
}
fn file_descriptor(value: Value) -> Result<FileDescriptor, String> {
    let file: FileDescriptor = serde_json::from_value(value).map_err(|_| "Forge 文件描述无效。")?;
    let url = Url::parse(&file.download_url).map_err(|_| "Forge 下载地址无效。")?;
    let segments: Vec<String> = url
        .path_segments()
        .ok_or("Forge 下载路径无效。")?
        .map(|part| {
            percent_encoding::percent_decode_str(part)
                .decode_utf8()
                .map(|value| value.into_owned())
                .map_err(|_| "Forge 下载路径无效。".to_string())
        })
        .collect::<Result<_, _>>()?;
    if file.name.is_empty()
        || !(file.name.ends_with(".zlib") || file.name.ends_with(".update.tar"))
        || file.name.contains(['/', '\\'])
        || file.name.chars().any(char::is_control)
        || file.size == 0
        || file.size > MAX_BYTES
        || url.scheme() != "https"
        || url.host_str() != Some("static-volc.gizclaw.com")
        || url.port().is_some()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
        || !url.path().starts_with("/releases/h2vivi/firmwares/")
        || segments.iter().any(|part| {
            part.is_empty()
                || part == "."
                || part == ".."
                || part.contains(['/', '\\'])
                || part.chars().any(char::is_control)
        })
        || segments.last() != Some(&file.name)
    {
        return Err("Forge 下载地址或文件大小无效。".into());
    }
    if file.sha256.as_ref().is_some_and(|digest| {
        digest.len() != 64
            || !digest
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    }) {
        return Err("Forge SHA-256 无效。".into());
    }
    Ok(file)
}

fn callback(url: &Url, state: &str, expires: u64) -> Result<Option<FileDescriptor>, String> {
    if url.origin().ascii_serialization() != "https://www.h6infra.com"
        || url.path() != "/tools/forge-picker/callback"
        || url.query().is_some()
    {
        return Err("Forge 回调地址无效。".into());
    }
    let pairs: Vec<_> =
        url::form_urlencoded::parse(url.fragment().ok_or("Forge 回调缺少结果。")?.as_bytes())
            .collect();
    if pairs.len() != 1 || pairs[0].0 != "forgePicker" || pairs[0].1.len() > 16384 {
        return Err("Forge 回调结果无效。".into());
    }
    let result: Value = serde_json::from_str(&pairs[0].1).map_err(|_| "Forge 回调结果无效。")?;
    if result["protocol"] != "gizclaw.forge-picker"
        || result["version"] != 1
        || result["type"] != "result"
        || result["state"] != state
        || result["expiresAt"] != expires
        || result["callbackUrl"] != CALLBACK
        || now_ms() >= expires
    {
        return Err("Forge 选择已失效，请重新选择。".into());
    }
    match result["status"].as_str() {
        Some("cancelled") if result.get("file").is_none() => Ok(None),
        Some("selected") => file_descriptor(result["file"].clone()).map(Some),
        _ => Err("Forge 回调状态无效。".into()),
    }
}

pub async fn choose(
    app: &tauri::AppHandle,
    cancel: &CancellationToken,
    progress: &Channel<Value>,
) -> Result<Option<package::Package>, String> {
    let state = format!(
        "{}{}",
        uuid::Uuid::new_v4().simple(),
        uuid::Uuid::new_v4().simple()
    );
    let expires = now_ms() + 5 * 60_000;
    let mut url = Url::parse("https://forge.gizclaw.com/console/picker/").unwrap();
    url.query_pairs_mut()
        .append_pair("state", &state)
        .append_pair("expires_at", &expires.to_string())
        .append_pair("callback", CALLBACK)
        .append_pair("path", "releases/h2vivi/firmwares/")
        .append_pair("extensions", ".zlib,.update.tar");
    let label = format!("forge-{}", uuid::Uuid::new_v4().simple());
    let (send, receive) = oneshot::channel::<Result<Option<FileDescriptor>, String>>();
    let send = Arc::new(Mutex::new(Some(send)));
    let navigation_sender = send.clone();
    let window = WebviewWindowBuilder::new(app, &label, WebviewUrl::External(url))
        .title("Forge · 选择固件")
        .inner_size(1100., 800.)
        .min_inner_size(720., 560.)
        .on_navigation(move |url| {
            if url.origin().ascii_serialization() == "https://www.h6infra.com"
                && url.path() == "/tools/forge-picker/callback"
            {
                if let Some(send) = navigation_sender.lock().unwrap().take() {
                    let _ = send.send(callback(url, &state, expires));
                }
                return false;
            }
            true
        })
        .build()
        .map_err(|error| error.to_string())?;
    let close_sender = send.clone();
    window.on_window_event(move |event| {
        if matches!(event, WindowEvent::Destroyed) {
            if let Some(send) = close_sender.lock().unwrap().take() {
                let _ = send.send(Ok(None));
            }
        }
    });
    let result = tokio::select! {
        _ = cancel.cancelled() => Err("已取消固件选择。".to_string()),
        _ = tokio::time::sleep(Duration::from_secs(300)) => Err("Forge 选择已超时，请重新选择。".to_string()),
        result = receive => result.map_err(|_| "Forge 选择窗口已关闭。".to_string())?,
    };
    let _ = window.close();
    let Some(file) = result? else {
        return Ok(None);
    };
    download::package(
        download::Request {
            url: file.download_url,
            name: Some(file.name),
            size: Some(file.size),
            sha256: file.sha256,
            redirects: false,
        },
        cancel,
        progress,
    )
    .await
    .map(Some)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn public_descriptor_rejects_foreign_origin_paths_and_sizes() {
        let valid = json!({ "name": "app.tar.zlib", "size": 100, "downloadUrl": "https://static-volc.gizclaw.com/releases/h2vivi/firmwares/test/1.0.0/app.tar.zlib" });
        assert!(file_descriptor(valid.clone()).is_ok());
        for url in [
            "https://attacker.invalid/releases/h2vivi/firmwares/app.tar.zlib",
            "https://static-volc.gizclaw.com/credentials/app.tar.zlib",
            "https://static-volc.gizclaw.com/releases/h2vivi/firmwares/a/app.tar.zlib?token=x",
        ] {
            let mut value = valid.clone();
            value["downloadUrl"] = url.into();
            assert!(file_descriptor(value).is_err());
        }
        let mut value = valid;
        value["size"] = (MAX_BYTES + 1).into();
        assert!(file_descriptor(value).is_err());
    }

    #[test]
    fn callback_binds_result_to_the_live_selection() {
        let state = "a".repeat(64);
        let expires = now_ms() + 60_000;
        let valid = json!({ "protocol": "gizclaw.forge-picker", "version": 1,
        "type": "result", "state": state, "expiresAt": expires,
        "callbackUrl": CALLBACK, "status": "selected", "file": {
            "name": "app.tar.zlib", "size": 100,
            "downloadUrl": "https://static-volc.gizclaw.com/releases/h2vivi/firmwares/test/1.0.0/app.tar.zlib"
        }});
        let make_url = |result: &Value| {
            let mut url = Url::parse(CALLBACK).unwrap();
            url.set_fragment(Some(
                &url::form_urlencoded::Serializer::new(String::new())
                    .append_pair("forgePicker", &result.to_string())
                    .finish(),
            ));
            url
        };
        assert!(callback(&make_url(&valid), &state, expires)
            .unwrap()
            .is_some());
        for (field, value) in [
            ("state", json!("other")),
            ("expiresAt", json!(0)),
            ("callbackUrl", json!("https://attacker.invalid/callback")),
            ("version", json!(2)),
        ] {
            let mut invalid = valid.clone();
            invalid[field] = value;
            assert!(callback(&make_url(&invalid), &state, expires).is_err());
        }
        let mut cancelled = valid;
        cancelled["status"] = "cancelled".into();
        cancelled.as_object_mut().unwrap().remove("file");
        assert!(callback(&make_url(&cancelled), &state, expires)
            .unwrap()
            .is_none());
    }
}
