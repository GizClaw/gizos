use serde::{Deserialize, Serialize};
use std::{
    fs::File,
    io,
    path::Path,
    process::{Command, Stdio},
    time::{Duration, Instant},
};
use tempfile::NamedTempFile;

pub const MAX_COMPRESSED_BYTES: u64 = 256 * 1024 * 1024;
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct Metadata {
    pub role: String,
    pub board: String,
    pub target: String,
    pub version: String,
}
// The private snapshot stays alive while any job holds this package.
pub struct Package {
    pub file: NamedTempFile,
    pub name: String,
    pub size: u64,
    pub metadata: Metadata,
}
fn snapshot_file() -> io::Result<NamedTempFile> {
    let mut builder = tempfile::Builder::new();
    builder.prefix("h2loader-package-");
    #[cfg(unix)]
    {
        builder.tempfile_in("/tmp")
    }
    #[cfg(not(unix))]
    {
        builder.tempfile()
    }
}
pub fn inspect(path: &Path) -> Result<Package, String> {
    if !path.is_absolute() {
        return Err("请选择本机固件包。".into());
    }
    let name = path
        .file_name()
        .and_then(|v| v.to_str())
        .ok_or("无效的文件名。")?
        .to_string();
    let source = File::open(path).map_err(|e| e.to_string())?;
    let size = source.metadata().map_err(|e| e.to_string())?.len();
    if size == 0 || size > MAX_COMPRESSED_BYTES {
        return Err("固件包大小无效。".into());
    }
    let mut snapshot = snapshot_file().map_err(|e| e.to_string())?;
    use std::io::Read;
    if io::copy(&mut source.take(size + 1), &mut snapshot).map_err(|e| e.to_string())? != size {
        return Err("固件包在读取时发生变化，请重新选择。".into());
    }
    snapshot.as_file().sync_all().map_err(|e| e.to_string())?;
    let mut child = Command::new(crate::sessions::binary()?)
        .arg("--inspect")
        .arg(snapshot.path())
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| e.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) => {}
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(error.to_string());
            }
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            return Err("固件包校验超时。".into());
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    let output = child.wait_with_output().map_err(|e| e.to_string())?;
    if !output.status.success() {
        return Err(format!(
            "固件包校验失败：{}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let metadata =
        serde_json::from_slice(&output.stdout).map_err(|e| format!("固件元数据无效：{e}"))?;
    Ok(Package {
        file: snapshot,
        name,
        size,
        metadata,
    })
}
#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{write::ZlibEncoder, Compression};
    use sha2::{Digest, Sha256};
    use std::io::Write;
    fn digest(hash: Sha256) -> String {
        format!("{:x}", hash.finalize())
    }

    fn fixture(role: &str, bad_image: bool, data: bool) -> NamedTempFile {
        fixture_identity(role, "test_board", "esp32s3", "1.2.3", bad_image, data)
    }

    fn fixture_identity(
        role: &str,
        board: &str,
        target: &str,
        version: &str,
        bad_image: bool,
        data: bool,
    ) -> NamedTempFile {
        let image = b"desktop-test-image";
        let manifest = format!("format=1\nrole={role}\nboard={board}\ntarget={target}\nversion={version}\nimage_size={}\nimage_sha256={}\n", image.len(), if bad_image { "0".repeat(64) } else { digest(Sha256::new_with_prefix(image)) });
        let mut entries = vec![
            ("manifest", manifest.into_bytes()),
            (
                "checksum",
                if data {
                    format!(
                        "{}\n",
                        digest(Sha256::new_with_prefix(b"data/a\0resource\0"))
                    )
                    .into_bytes()
                } else {
                    format!("{}\n", digest(Sha256::new())).into_bytes()
                },
            ),
        ];
        if data {
            entries.push(("data/a", b"resource".to_vec()));
        }
        entries.push(("app/esp/app.bin", image.to_vec()));
        let mut archive = tar::Builder::new(Vec::new());
        for (name, bytes) in entries {
            let mut header = tar::Header::new_ustar();
            header.set_size(bytes.len() as u64);
            header.set_mode(0o644);
            header.set_cksum();
            archive
                .append_data(&mut header, name, bytes.as_slice())
                .unwrap();
        }
        let mut encoder = ZlibEncoder::new(Vec::new(), Compression::default());
        encoder.write_all(&archive.into_inner().unwrap()).unwrap();
        let mut file = NamedTempFile::new().unwrap();
        file.write_all(&encoder.finish().unwrap()).unwrap();
        file
    }

    #[test]
    fn version_build_metadata_is_accepted_without_relaxing_board_or_target() {
        for role in ["app", "h2loader"] {
            let version = "0.10.7-prod.1+tiga-esp-v4-2";
            let file = fixture_identity(role, "test_board", "esp32s3", version, false, false);
            assert_eq!(inspect(file.path()).unwrap().metadata.version, version);
            for (board, target) in [("test+board", "esp32s3"), ("test_board", "esp32+s3")] {
                let file = fixture_identity(role, board, target, version, false, false);
                assert!(inspect(file.path()).is_err());
            }
            for invalid in ["", "1.2.3+bad value", "1.2.3+bad/path"] {
                let file = fixture_identity(role, "test_board", "esp32s3", invalid, false, false);
                assert!(inspect(file.path()).is_err());
            }
        }
    }

    #[test]
    fn detects_both_roles_and_validates_image_and_resources() {
        for role in ["app", "h2loader"] {
            assert_eq!(
                inspect(fixture(role, false, false).path())
                    .unwrap()
                    .metadata
                    .role,
                role
            );
        }
        assert!(inspect(fixture("app", false, true).path()).is_ok());
        assert!(inspect(fixture("h2loader", false, true).path()).is_err());
        assert!(inspect(fixture("app", true, false).path()).is_err());
        assert!(inspect(fixture("unknown", false, false).path()).is_err());
    }

    #[test]
    fn snapshot_is_immutable_and_extra_compressed_bytes_are_rejected() {
        let file = fixture("app", false, false);
        let package = inspect(file.path()).unwrap();
        std::fs::write(file.path(), b"replacement").unwrap();
        assert!(inspect(package.file.path()).is_ok());
        let mut file = fixture("app", false, false);
        file.write_all(b"junk").unwrap();
        assert!(inspect(file.path()).is_err());
    }

    #[tokio::test]
    async fn retained_snapshot_is_readable_by_the_bundled_native_sdk() {
        let source = fixture("app", false, false);
        let package = inspect(source.path()).unwrap();
        let output = snapshot_file().unwrap();
        let args = vec![
            "--no-ble".into(),
            "package".into(),
            "--app-bin".into(),
            package.file.path().to_string_lossy().to_string(),
            "--out".into(),
            output.path().to_string_lossy().to_string(),
            "--board".into(),
            "fixture".into(),
            "--target".into(),
            "host".into(),
            "--version".into(),
            "0.1.0".into(),
        ];
        crate::native::run(
            &crate::native::binary().unwrap(),
            &args,
            &tokio_util::sync::CancellationToken::new(),
            |_| {},
        )
        .await
        .unwrap();
        assert!(output.as_file().metadata().unwrap().len() > 0);
        assert_eq!(inspect(output.path()).unwrap().metadata.version, "0.1.0");
    }

    #[test]
    fn compressed_size_limit_is_enforced_before_native_inspection() {
        let file = NamedTempFile::new().unwrap();
        file.as_file().set_len(MAX_COMPRESSED_BYTES + 1).unwrap();
        assert!(inspect(file.path()).is_err());
    }
}
