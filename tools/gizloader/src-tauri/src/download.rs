use crate::package;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{io::Write, time::Duration};
use tauri::ipc::Channel;
use tokio_util::sync::CancellationToken;
use url::Url;

pub const MAX_BYTES: u64 = package::MAX_COMPRESSED_BYTES;

pub struct Request {
    pub url: String,
    pub name: Option<String>,
    pub size: Option<u64>,
    pub sha256: Option<String>,
    pub redirects: bool,
}

fn https_url(value: &str) -> Result<Url, String> {
    let url = Url::parse(value).map_err(|_| "请输入有效的 HTTPS 固件包 URL。")?;
    if url.scheme() != "https"
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.fragment().is_some()
    {
        return Err("请输入有效的 HTTPS 固件包 URL。".into());
    }
    Ok(url)
}

pub async fn package(
    request: Request,
    cancel: &CancellationToken,
    progress: &Channel<Value>,
) -> Result<package::Package, String> {
    let url = https_url(&request.url)?;
    if cancel.is_cancelled() {
        return Err("已取消固件选择。".into());
    }
    let redirect = if request.redirects {
        reqwest::redirect::Policy::custom(|attempt| {
            if attempt.previous().len() >= 5 || https_url(attempt.url().as_str()).is_err() {
                attempt.error("固件下载重定向无效。")
            } else {
                attempt.follow()
            }
        })
    } else {
        reqwest::redirect::Policy::none()
    };
    let client = reqwest::Client::builder()
        .redirect(redirect)
        .timeout(Duration::from_secs(180))
        .build()
        .map_err(|error| error.without_url().to_string())?;
    let response = tokio::select! {
        _ = cancel.cancelled() => return Err("已取消固件选择。".into()),
        response = client.get(url).send() => response.map_err(|error| error.without_url().to_string())?,
    };
    let mut response = response
        .error_for_status()
        .map_err(|error| error.without_url().to_string())?;
    let length = response.content_length();
    if !response.status().is_success()
        || length.is_some_and(|size| size == 0 || size > MAX_BYTES)
        || request.size.is_some_and(|expected| {
            expected == 0 || expected > MAX_BYTES || length.is_some_and(|size| size != expected)
        })
    {
        return Err("固件包超过 256 MiB 或下载大小不匹配。".into());
    }
    let name = request.name.unwrap_or_else(|| {
        response
            .url()
            .path_segments()
            .and_then(|mut parts| parts.next_back())
            .and_then(|name| {
                percent_encoding::percent_decode_str(name)
                    .decode_utf8()
                    .ok()
            })
            .filter(|name| {
                name.ends_with(".zlib")
                    && !name.contains(['/', '\\'])
                    && !name.chars().any(char::is_control)
            })
            .map(|name| name.into_owned())
            .unwrap_or_else(|| "firmware.tar.zlib".into())
    });
    let total = request.size.or(length);
    let mut temporary = tempfile::NamedTempFile::new().map_err(|error| error.to_string())?;
    let mut received = 0u64;
    let mut sha = Sha256::new();
    loop {
        let chunk = tokio::select! {
            _ = cancel.cancelled() => return Err("已取消固件选择。".into()),
            chunk = response.chunk() => chunk.map_err(|error| error.without_url().to_string())?,
        };
        let Some(chunk) = chunk else {
            break;
        };
        received += chunk.len() as u64;
        if received > MAX_BYTES || total.is_some_and(|size| received > size) {
            return Err("固件包超过 256 MiB 或下载大小不匹配。".into());
        }
        temporary
            .write_all(&chunk)
            .map_err(|error| error.to_string())?;
        sha.update(&chunk);
        progress
            .send(json!({ "receivedBytes": received, "totalBytes": total,
            "percent": total.map(|size| received as f64 * 100. / size as f64) }))
            .map_err(|_| "固件选择已关闭。")?;
    }
    if received == 0
        || total.is_some_and(|size| received != size)
        || request
            .sha256
            .is_some_and(|expected| expected != format!("{:x}", sha.finalize()))
    {
        return Err("固件包下载校验失败。".into());
    }
    let mut package =
        tauri::async_runtime::spawn_blocking(move || package::inspect(temporary.path()))
            .await
            .map_err(|error| error.to_string())??;
    if cancel.is_cancelled() {
        return Err("已取消固件选择。".into());
    }
    package.name = name;
    Ok(package)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn url_downloads_require_https_without_embedded_credentials_or_fragments() {
        assert!(
            https_url("https://static-volc.gizclaw.com/releases/test/app.tar.zlib?download=1")
                .is_ok()
        );
        for url in [
            "http://example.com/app.tar.zlib",
            "file:///etc/passwd",
            "https://user:secret@example.com/app.tar.zlib",
            "https://example.com/app.tar.zlib#fragment",
        ] {
            assert!(https_url(url).is_err());
        }
    }
}
