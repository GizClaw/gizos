use flate2::bufread::ZlibDecoder;
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    fs::File,
    io::{self, BufReader, Read, Seek},
    path::Path,
};
use tempfile::NamedTempFile;

pub const MAX_COMPRESSED_BYTES: u64 = 256 * 1024 * 1024;
const MAX_EXPANDED_BYTES: u64 = 512 * 1024 * 1024;

// Count the complete tar stream, including headers, padding and resources.
struct BoundedExpansion<R> {
    inner: R,
    remaining: u64,
}
impl<R: Read> Read for BoundedExpansion<R> {
    fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
        let limit = bytes.len().min(self.remaining.saturating_add(1) as usize);
        let read = self.inner.read(&mut bytes[..limit])?;
        if read as u64 > self.remaining {
            return Err(io::Error::other("固件包解压后超过大小限制。"));
        }
        self.remaining -= read as u64;
        Ok(read)
    }
}

#[derive(Clone, Debug, Serialize)]
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

fn text(entry: &mut impl Read, limit: usize) -> Result<String, String> {
    let mut bytes = Vec::new();
    entry
        .take((limit + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    if bytes.len() > limit {
        return Err("固件包元数据过长。".into());
    }
    String::from_utf8(bytes).map_err(|_| "固件包元数据不是 UTF-8。".into())
}

fn hash_entry(entry: &mut impl Read, hash: &mut Sha256) -> Result<u64, String> {
    let mut buffer = [0u8; 16384];
    let mut size = 0u64;
    loop {
        let count = entry.read(&mut buffer).map_err(|e| e.to_string())?;
        if count == 0 {
            break;
        }
        hash.update(&buffer[..count]);
        size += count as u64;
    }
    Ok(size)
}

fn digest(hash: Sha256) -> String {
    format!("{:x}", hash.finalize())
}
fn safe_text(value: &str, punctuation: &[u8]) -> bool {
    !value.is_empty()
        && value.len() < 128
        && value
            .bytes()
            .all(|v| v.is_ascii_alphanumeric() || punctuation.contains(&v))
}

pub fn inspect(path: &Path) -> Result<Package, String> {
    inspect_bounded(path, MAX_EXPANDED_BYTES)
}

fn snapshot_file() -> io::Result<NamedTempFile> {
    let mut builder = tempfile::Builder::new();
    builder.prefix("gizloader-package-");
    // Native Unix SDK compositions mount /tmp. The process's TMPDIR can be
    // elsewhere (Darwin /var/folders or a custom build/cache volume).
    #[cfg(unix)]
    {
        builder.tempfile_in("/tmp")
    }
    #[cfg(not(unix))]
    {
        builder.tempfile()
    }
}

fn inspect_bounded(path: &Path, expanded_limit: u64) -> Result<Package, String> {
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
    let copied =
        std::io::copy(&mut source.take(size + 1), &mut snapshot).map_err(|e| e.to_string())?;
    if copied != size {
        return Err("固件包在读取时发生变化，请重新选择。".into());
    }
    snapshot.as_file().sync_all().map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(File::open(snapshot.path()).map_err(|e| e.to_string())?);
    let mut prefix = [0; 9];
    reader.read_exact(&mut prefix).map_err(|e| e.to_string())?;
    reader.rewind().map_err(|e| e.to_string())?;
    let metadata = if &prefix == b"manifest\0" {
        inspect_format2(&mut reader, size, expanded_limit)?
    } else {
        inspect_format1(reader, size, expanded_limit)?
    };
    Ok(Package {
        file: snapshot,
        name,
        size,
        metadata,
    })
}

fn inspect_format1(
    reader: impl io::BufRead,
    size: u64,
    expanded_limit: u64,
) -> Result<Metadata, String> {
    let decoder = ZlibDecoder::new(reader);
    let mut archive = tar::Archive::new(BoundedExpansion {
        inner: decoder,
        remaining: expanded_limit,
    });
    let mut metadata = None;
    let mut expected_image_size = 0;
    let mut expected_image_hash = String::new();
    let mut expected_data_hash = String::new();
    let mut data_hash = Sha256::new();
    let mut last_data_path = String::new();
    let mut image_seen = false;
    for (index, entry) in archive
        .entries()
        .map_err(|e| e.to_string())?
        .raw(true)
        .enumerate()
    {
        let mut entry = entry.map_err(|e| e.to_string())?;
        if entry.size() > expanded_limit {
            return Err("固件包解压后超过大小限制。".into());
        }
        if !entry.header().entry_type().is_file() || image_seen {
            return Err("固件包包含不支持的条目。".into());
        }
        let entry_path =
            String::from_utf8(entry.path_bytes().to_vec()).map_err(|_| "无效的包路径。")?;
        if entry_path
            .split('/')
            .any(|part| part.is_empty() || part == "." || part == "..")
        {
            return Err("无效的包路径。".into());
        }
        match index {
            0 => {
                if entry_path != "manifest" {
                    return Err("固件包缺少 manifest。".into());
                }
                let manifest = text(&mut entry, 2048)?;
                let expected = [
                    "format",
                    "role",
                    "board",
                    "target",
                    "version",
                    "image_size",
                    "image_sha256",
                ];
                let lines: Vec<_> = manifest.lines().collect();
                if lines.len() != expected.len() || !manifest.ends_with('\n') {
                    return Err("固件包 manifest 格式无效。".into());
                }
                let values: Vec<&str> = lines
                    .iter()
                    .zip(expected)
                    .map(|(line, key)| {
                        line.strip_prefix(&format!("{key}="))
                            .ok_or("固件包 manifest 字段无效。".to_string())
                    })
                    .collect::<Result<_, _>>()?;
                if values[0] != "1"
                    || !["app", "h2loader"].contains(&values[1])
                    || !values[2..4].iter().all(|value| safe_text(value, b"._-"))
                    || !safe_text(values[4], b"._-+")
                {
                    return Err("固件包类型或身份无效。".into());
                }
                expected_image_size = values[5].parse::<u64>().map_err(|_| "固件镜像大小无效。")?;
                if expected_image_size == 0 || expected_image_size > expanded_limit {
                    return Err("固件镜像大小超过限制。".into());
                }
                expected_image_hash = values[6].to_string();
                metadata = Some(Metadata {
                    role: values[1].into(),
                    board: values[2].into(),
                    target: values[3].into(),
                    version: values[4].into(),
                });
            }
            1 => {
                if entry_path != "checksum" {
                    return Err("固件包缺少 checksum。".into());
                }
                expected_data_hash = text(&mut entry, 65)?.trim_end_matches('\n').to_string();
                if expected_data_hash.len() != 64 {
                    return Err("固件包 checksum 无效。".into());
                }
            }
            _ if entry_path.starts_with("data/") => {
                if metadata.as_ref().unwrap().role != "app" || entry_path <= last_data_path {
                    return Err("固件包资源顺序或类型无效。".into());
                }
                data_hash.update(entry_path.as_bytes());
                data_hash.update([0]);
                hash_entry(&mut entry, &mut data_hash)?;
                data_hash.update([0]);
                last_data_path = entry_path;
            }
            _ if entry_path.starts_with("app/") => {
                if entry.size() != expected_image_size {
                    return Err("固件镜像大小不匹配。".into());
                }
                if digest(data_hash.clone()) != expected_data_hash {
                    return Err("固件包资源校验失败。".into());
                }
                let mut image_hash = Sha256::new();
                let image_size = hash_entry(&mut entry, &mut image_hash)?;
                if image_size == 0
                    || image_size != expected_image_size
                    || digest(image_hash) != expected_image_hash
                {
                    return Err("固件镜像校验失败。".into());
                }
                image_seen = true;
            }
            _ => return Err("固件包包含未知条目。".into()),
        }
    }
    if !image_seen {
        return Err("固件包缺少镜像。".into());
    }
    // Finish decompression to validate the zlib checksum and reject trailing data.
    let mut decoder = archive.into_inner();
    let mut buffer = [0; 16384];
    let mut zero_bytes = 0;
    loop {
        let count = decoder.read(&mut buffer).map_err(|e| e.to_string())?;
        if count == 0 {
            break;
        }
        if buffer[..count].iter().any(|byte| *byte != 0) {
            return Err("固件包尾部无效。".into());
        }
        zero_bytes += count;
    }
    if zero_bytes < 512 || zero_bytes % 512 != 0 || decoder.inner.total_in() != size {
        return Err("固件包被截断或包含额外数据。".into());
    }
    Ok(metadata.unwrap())
}

struct Format2Manifest {
    metadata: Metadata,
    app_zlib_size: u64,
    app_zlib_sha256: String,
    data_zlib_size: u64,
    data_zlib_sha256: String,
}

fn parse_format2_manifest(manifest: &str, expanded_limit: u64) -> Result<Format2Manifest, String> {
    let expected = [
        "format",
        "role",
        "board",
        "target",
        "version",
        "image_size",
        "image_sha256",
        "data_sha256",
        "data_tar_size",
        "data_bytes",
        "pixa_bytes",
        "app_zlib_size",
        "app_zlib_sha256",
        "data_zlib_size",
        "data_zlib_sha256",
    ];
    let lines: Vec<_> = manifest.split_terminator('\n').collect();
    if lines.len() != expected.len() || !manifest.ends_with('\n') {
        return Err("固件包 manifest 格式无效。".into());
    }
    let values: Vec<_> = lines
        .iter()
        .zip(expected)
        .map(|(line, key)| {
            line.strip_prefix(&format!("{key}="))
                .filter(|value| !value.is_empty())
                .ok_or("固件包 manifest 字段无效。".to_string())
        })
        .collect::<Result<_, _>>()?;
    if values[0] != "2"
        || !["app", "h2loader"].contains(&values[1])
        || !values[2..4].iter().all(|value| {
            value.len() < 96
                && value
                    .bytes()
                    .all(|v| v.is_ascii_lowercase() || v.is_ascii_digit() || b"._-".contains(&v))
        })
        || values[4].len() >= 96
        || !values[4].bytes().all(|v| (0x21..=0x7e).contains(&v))
    {
        return Err("固件包类型或身份无效。".into());
    }
    for index in [6, 7, 12, 14] {
        if values[index].len() != 64
            || !values[index]
                .bytes()
                .all(|v| v.is_ascii_digit() || (b'a'..=b'f').contains(&v))
        {
            return Err("固件包 SHA-256 无效。".into());
        }
    }
    let number = |index: usize| -> Result<u64, String> {
        let value = values[index];
        if !value.bytes().all(|v| v.is_ascii_digit()) {
            return Err("固件包大小无效。".into());
        }
        value.parse().map_err(|_| "固件包大小无效。".into())
    };
    let image_size = number(5)?;
    let data_tar_size = number(8)?;
    let data_bytes = number(9)?;
    let pixa_bytes = number(10)?;
    let app_zlib_size = number(11)?;
    let data_zlib_size = number(13)?;
    if image_size == 0
        || data_tar_size < 1024
        || data_tar_size % 512 != 0
        || app_zlib_size < 6
        || data_zlib_size < 6
        || data_bytes > data_tar_size
        || pixa_bytes > data_tar_size - data_bytes
        || (values[1] == "h2loader" && values[7] != digest(Sha256::new()))
    {
        return Err("固件包 manifest 大小或资源类型无效。".into());
    }
    if image_size
        .checked_add(data_tar_size)
        .is_none_or(|size| size > expanded_limit)
    {
        return Err("固件包解压后超过大小限制。".into());
    }
    Ok(Format2Manifest {
        metadata: Metadata {
            role: values[1].into(),
            board: values[2].into(),
            target: values[3].into(),
            version: values[4].into(),
        },
        app_zlib_size,
        app_zlib_sha256: values[12].into(),
        data_zlib_size,
        data_zlib_sha256: values[14].into(),
    })
}

fn tar_padding(size: u64) -> u64 {
    (512 - size % 512) % 512
}

fn format2_member(reader: &mut impl Read, remaining: &mut u64, name: &str) -> Result<u64, String> {
    if *remaining < 512 {
        return Err("固件包被截断。".into());
    }
    let mut block = [0; 512];
    reader.read_exact(&mut block).map_err(|e| e.to_string())?;
    *remaining -= 512;
    let header = tar::Header::from_byte_slice(&block);
    let checksum: u32 = block
        .iter()
        .enumerate()
        .map(|(index, byte)| {
            u32::from(if (148..156).contains(&index) {
                b' '
            } else {
                *byte
            })
        })
        .sum();
    if header.as_ustar().is_none()
        || !header.entry_type().is_file()
        || header.path_bytes().as_ref() != name.as_bytes()
        || header.cksum().map_err(|e| e.to_string())? != checksum
    {
        return Err("固件包条目或 tar 校验无效。".into());
    }
    let size = header.size().map_err(|e| e.to_string())?;
    if size == 0 || size > *remaining || tar_padding(size) > *remaining - size {
        return Err("固件包条目大小无效。".into());
    }
    Ok(size)
}

fn read_zero_bytes(reader: &mut impl Read, mut size: u64) -> Result<(), String> {
    let mut buffer = [0; 16384];
    while size != 0 {
        let count = size.min(buffer.len() as u64) as usize;
        reader
            .read_exact(&mut buffer[..count])
            .map_err(|e| e.to_string())?;
        if buffer[..count].iter().any(|byte| *byte != 0) {
            return Err("固件包 padding 或尾部无效。".into());
        }
        size -= count as u64;
    }
    Ok(())
}

fn inspect_format2(
    reader: &mut impl Read,
    size: u64,
    expanded_limit: u64,
) -> Result<Metadata, String> {
    if size < 2048 || !size.is_multiple_of(512) {
        return Err("固件包 tar 大小无效。".into());
    }
    let mut remaining = size;
    let manifest_size = format2_member(reader, &mut remaining, "manifest")?;
    if manifest_size > 1024 {
        return Err("固件包元数据过长。".into());
    }
    let manifest = text(&mut (&mut *reader).take(manifest_size), 1024)?;
    let manifest = parse_format2_manifest(&manifest, expanded_limit)?;
    read_zero_bytes(reader, tar_padding(manifest_size))?;
    remaining -= manifest_size + tar_padding(manifest_size);

    // Match Host Core inspection: authenticate compressed members without
    // inflating them. Installation validates their raw image/data digests.
    for (name, expected_size, expected_hash) in [
        (
            "data.tar.zlib",
            manifest.data_zlib_size,
            &manifest.data_zlib_sha256,
        ),
        (
            "app.bin.zlib",
            manifest.app_zlib_size,
            &manifest.app_zlib_sha256,
        ),
    ] {
        let member_size = format2_member(reader, &mut remaining, name)?;
        if member_size != expected_size {
            return Err("固件包压缩条目大小不匹配。".into());
        }
        let mut hash = Sha256::new();
        if hash_entry(&mut (&mut *reader).take(member_size), &mut hash)? != member_size
            || digest(hash) != *expected_hash
        {
            return Err("固件包压缩条目校验失败。".into());
        }
        read_zero_bytes(reader, tar_padding(member_size))?;
        remaining -= member_size + tar_padding(member_size);
    }
    if remaining < 1024 {
        return Err("固件包被截断。".into());
    }
    read_zero_bytes(reader, remaining)?;
    if reader.read(&mut [0]).map_err(|e| e.to_string())? != 0 {
        return Err("固件包包含额外数据。".into());
    }
    Ok(manifest.metadata)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{write::ZlibEncoder, Compression};
    use std::io::Write;

    fn fixture(role: &str, bad_image: bool, data: bool) -> NamedTempFile {
        fixture_version(role, bad_image, data, "1.2.3")
    }

    fn fixture_version(role: &str, bad_image: bool, data: bool, version: &str) -> NamedTempFile {
        let image = b"desktop-test-image";
        let manifest = format!("format=1\nrole={role}\nboard=test_board\ntarget=esp32s3\nversion={version}\nimage_size={}\nimage_sha256={}\n", image.len(), if bad_image { "0".repeat(64) } else { digest(Sha256::new_with_prefix(image)) });
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

    fn fixture_format2(role: &str, data: bool) -> NamedTempFile {
        let image = b"desktop-test-image";
        let mut data_tar = tar::Builder::new(Vec::new());
        if data {
            let mut header = tar::Header::new_ustar();
            header.set_size(8);
            header.set_mode(0o644);
            header.set_cksum();
            data_tar
                .append_data(&mut header, "data/a", &b"resource"[..])
                .unwrap();
        }
        let data_tar = data_tar.into_inner().unwrap();
        let compress = |bytes: &[u8]| {
            let mut encoder = ZlibEncoder::new(Vec::new(), Compression::default());
            encoder.write_all(bytes).unwrap();
            encoder.finish().unwrap()
        };
        let app_zlib = compress(image);
        let data_zlib = compress(&data_tar);
        let data_sha256 = if data {
            digest(Sha256::new_with_prefix(b"data/a\0resource\0"))
        } else {
            digest(Sha256::new())
        };
        let manifest = format!(
            "format=2\nrole={role}\nboard=test_board\ntarget=esp32s3\nversion=0.10.7-prod.1+tiga-esp-v4-2\nimage_size={}\nimage_sha256={}\ndata_sha256={data_sha256}\ndata_tar_size={}\ndata_bytes={}\npixa_bytes=0\napp_zlib_size={}\napp_zlib_sha256={}\ndata_zlib_size={}\ndata_zlib_sha256={}\n",
            image.len(),
            digest(Sha256::new_with_prefix(image)),
            data_tar.len(),
            if data { 8 } else { 0 },
            app_zlib.len(),
            digest(Sha256::new_with_prefix(&app_zlib)),
            data_zlib.len(),
            digest(Sha256::new_with_prefix(&data_zlib)),
        );
        let mut archive = tar::Builder::new(Vec::new());
        for (name, bytes) in [
            ("manifest", manifest.into_bytes()),
            ("data.tar.zlib", data_zlib),
            ("app.bin.zlib", app_zlib),
        ] {
            let mut header = tar::Header::new_ustar();
            header.set_size(bytes.len() as u64);
            header.set_mode(0o644);
            header.set_cksum();
            archive
                .append_data(&mut header, name, bytes.as_slice())
                .unwrap();
        }
        let mut file = NamedTempFile::new().unwrap();
        file.write_all(&archive.into_inner().unwrap()).unwrap();
        file
    }

    fn format2_members(bytes: &[u8]) -> Vec<(usize, usize)> {
        let mut offset = 0;
        (0..3)
            .map(|_| {
                let size = tar::Header::from_byte_slice(&bytes[offset..offset + 512])
                    .size()
                    .unwrap() as usize;
                let start = offset + 512;
                offset = start + size + tar_padding(size as u64) as usize;
                (start, size)
            })
            .collect()
    }

    #[test]
    fn format2_detects_both_roles_and_validates_compressed_checksums() {
        for (role, data) in [("app", false), ("app", true), ("h2loader", false)] {
            let file = fixture_format2(role, data);
            let package = inspect(file.path()).unwrap();
            assert_eq!(package.metadata.role, role);
            assert_eq!(package.metadata.board, "test_board");
            assert_eq!(package.metadata.target, "esp32s3");
            assert_eq!(package.metadata.version, "0.10.7-prod.1+tiga-esp-v4-2");
            assert_eq!(package.size, file.as_file().metadata().unwrap().len());
        }
        assert!(inspect(fixture_format2("h2loader", true).path()).is_err());
        assert!(inspect(fixture_format2("unknown", false).path()).is_err());

        for member in [1, 2] {
            let file = fixture_format2("app", true);
            let mut bytes = std::fs::read(file.path()).unwrap();
            let members = format2_members(&bytes);
            bytes[members[member].0] ^= 1;
            std::fs::write(file.path(), bytes).unwrap();
            assert!(inspect(file.path()).err().unwrap().contains("校验失败"));
        }
    }

    #[test]
    fn format2_rejects_corrupt_headers_padding_and_trailing_bytes() {
        for member in 0..3 {
            for corrupt_padding in [false, true] {
                let file = fixture_format2("app", true);
                let mut bytes = std::fs::read(file.path()).unwrap();
                let members = format2_members(&bytes);
                let (start, size) = members[member];
                assert_ne!(size % 512, 0);
                let offset = if corrupt_padding {
                    start + size
                } else {
                    start - 512 + 100
                };
                bytes[offset] ^= 1;
                std::fs::write(file.path(), bytes).unwrap();
                assert!(inspect(file.path()).is_err());
            }
        }
        for trailing in [vec![1; 512], vec![0; 1]] {
            let mut file = fixture_format2("app", true);
            file.write_all(&trailing).unwrap();
            assert!(inspect(file.path()).is_err());
        }
        for truncate in [1, 512, 1024] {
            let file = fixture_format2("app", true);
            let size = file.as_file().metadata().unwrap().len();
            file.as_file().set_len(size - truncate).unwrap();
            assert!(inspect(file.path()).is_err());
        }
    }

    #[test]
    fn format2_snapshot_and_declared_expansion_are_bounded() {
        let file = fixture_format2("app", true);
        let package = inspect(file.path()).unwrap();
        assert!(package
            .file
            .path()
            .file_name()
            .unwrap()
            .to_string_lossy()
            .starts_with("gizloader-package-"));
        std::fs::write(file.path(), b"replacement").unwrap();
        assert!(inspect(package.file.path()).is_ok());
        let file = fixture_format2("app", true);
        let expanded_size = 2048 + b"desktop-test-image".len() as u64;
        assert!(inspect_bounded(file.path(), expanded_size).is_ok());
        assert!(inspect_bounded(file.path(), expanded_size - 1)
            .err()
            .unwrap()
            .contains("大小限制"));
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
    fn preserves_build_metadata_and_legacy_versions() {
        for role in ["app", "h2loader"] {
            for version in [
                "0.10.7-prod.1+tiga-esp-v4-2",
                "0.10.0-1+zero-esp-v3-0",
                "legacy_20260928",
            ] {
                let file = fixture_version(role, false, false, version);
                assert_eq!(inspect(file.path()).unwrap().metadata.version, version);
            }
        }
        for version in [
            "",
            "bad/version",
            "bad version",
            "bad=version",
            "bad\tversion",
            "bad\u{80}",
        ] {
            let file = fixture_version("app", false, false, version);
            assert!(
                inspect(file.path()).is_err(),
                "accepted version: {version:?}"
            );
        }
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
        let version = "0.10.7-prod.1+tiga-esp-v4-2";
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
            version.into(),
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
        let generated = inspect(output.path()).unwrap();
        assert_eq!(generated.metadata.role, "app");
        assert_eq!(generated.metadata.board, "fixture");
        assert_eq!(generated.metadata.target, "host");
        assert_eq!(generated.metadata.version, version);
    }

    #[test]
    fn expanded_budget_covers_all_tar_entries_and_exact_eof() {
        let file = fixture("app", false, true);
        assert!(inspect_bounded(file.path(), 8192).is_ok());
        assert!(inspect_bounded(file.path(), 4096)
            .err()
            .unwrap()
            .contains("大小限制"));
        let mut reader = BoundedExpansion {
            inner: io::Cursor::new(vec![1u8; 10]),
            remaining: 10,
        };
        let mut output = Vec::new();
        assert_eq!(reader.read_to_end(&mut output).unwrap(), 10);
        let mut reader = BoundedExpansion {
            inner: io::Cursor::new(vec![1u8; 11]),
            remaining: 10,
        };
        assert!(reader.read_to_end(&mut Vec::new()).is_err());
    }
}
