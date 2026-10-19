"""Separate live artifact audits from captured qualification consistency checks."""
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess

AMOLED_R15_VERSION = "pal-wifi-fixture-20260929-r15"
AMOLED_R15_PACKAGE = "b753d84fb4ca0ce1bd85bd23d65713523a40c877b8a0ed3831a2bc37a8d7ec2a"
AMOLED_R15_IMAGE = "ff4ae67691fc4ee8e85a7fed8347c7215baf0041338f0d4792fd85318edfa8f7"
AMOLED_R15_HISTORICAL_SOURCES = {
    "native_component_src/esp-idf6.x/h2_pal_core/src/h2_esp_platform_wifi.c": {
        "commit": "2d48858a39a1f96361210651ccb4f76894750709",
        "sha256": "89798a6ad0ce9ac3a9cc35e4035993ce74e7884f17992ee6c84644fee94e8469"},
    "projects/e2e/apps/pal-wifi/app/include/h2_pal_wifi_e2e.h": {
        "commit": "de0afcf5b75966f9b1a3a7a3c1ff7b842d861c3e",
        "sha256": "cec595ed3b8d6263d889b3e587751284224dc3bacfeda37f60aa49263d9db487"},
    "projects/e2e/libs/pal-wifi-device/src/h2_pal_wifi_device.c": {
        "commit": "451afcf1c951d95cf08e3d9b1fbd6324c4742d2f",
        "sha256": "7e40c3b531541785a73c979204b14d7c725fc8da88f2797395bd351f5576bbbd"},
}
import tarfile
import zlib

P1_FIELDS = {"valid", "package_checksum", "package_size", "image_checksum",
             "image_size", "role", "version", "board", "target"}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def kv(path, marker):
    lines = [line for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
             if marker in line]
    assert len(lines) == 1, (path, marker, len(lines))
    pairs = re.findall(r"([a-zA-Z_0-9]+)=([^\s]+)", lines[0])
    assert len(dict(pairs)) == len(pairs), "duplicate status fields"
    return dict(pairs)


def bound_ref(ref):
    assert sha(ref["path"]) == ref["sha256"], ref["path"]
    return Path(ref["path"])


def _hash(value):
    assert isinstance(value, str) and re.fullmatch(r"[a-f0-9]{64}", value), "invalid SHA256"


def _historical_source_policy(report):
    historical = report.get("historical_source_inputs", {})
    if report.get("platform") == "amoled-fixture":
        assert report["version"] == AMOLED_R15_VERSION, "historical fixture version changed"
        assert report["package_sha256"] == AMOLED_R15_PACKAGE, "historical fixture package changed"
        assert report["image_sha256"] == AMOLED_R15_IMAGE, "historical fixture image changed"
        assert historical == AMOLED_R15_HISTORICAL_SOURCES, "unexpected historical source exception"
        for path, identity in historical.items():
            assert report["source_inputs"][path] == identity["sha256"], "historical source hash changed"
    else:
        assert not historical, "DUT or host cannot claim historical fixture source"
    return historical


def _verify_historical_git_blobs(historical):
    for path, identity in historical.items():
        raw = subprocess.check_output(["git", "show", identity["commit"] + ":" + path])
        assert hashlib.sha256(raw).hexdigest() == identity["sha256"], ("historical Git bytes", path)


def _check_sources(inputs, historical=None):
    assert inputs, "missing source snapshot"
    historical = historical or {}
    for path, expected in inputs.items():
        _hash(expected)
        if path in historical:
            assert expected == historical[path]["sha256"]
            continue
        assert sha(path) == expected, ("source drift", path)


def _check_snapshot(report, snapshot):
    assert snapshot["schema"] == 1 and snapshot["capture_method"] == "verified-local-package-and-loader-bytes"
    assert snapshot["uid"] == report["uid"] and snapshot["version"] == report["version"]
    expected_raw = {"firmware", "package", "baseline_status", "stage_status", "baseline_coredump"}
    if snapshot["baseline_coredump"]["blank"] == "0":
        expected_raw.add("baseline_coredump_bytes")
    assert snapshot["raw_hashes"].keys() == expected_raw, "incomplete raw identity snapshot"
    for field, value in snapshot["raw_hashes"].items():
        _hash(value)
        assert value == report[field]["sha256"], ("raw receipt identity", field)
    metadata = snapshot["metadata"]
    asset, manifest = metadata["assets"][0], metadata["package_manifest"]
    assert type(manifest["format"]) is int and manifest["format"] in (1, 2) and manifest["role"] == "app"
    assert manifest["version"] == report["version"]
    assert metadata["version"] == manifest["version"] and metadata["board"] == manifest["board"]
    assert metadata["target"] == manifest["target"]
    assert manifest["target"] in {"esp32s3", "bk7258"}
    package, image = snapshot["package"], snapshot["image"]
    for entry in (package, image):
        _hash(entry["sha256"])
        assert isinstance(entry["size"], int) and entry["size"] > 0
    assert package == {"sha256": asset["sha256"], "size": asset["size"]}
    assert image == {"sha256": manifest["image_sha256"], "size": manifest["image_size"]}
    assert package["sha256"] == report["package_sha256"] and image["sha256"] == report["image_sha256"]
    embedded = snapshot["archive_manifest"]
    expected = {k: str(v) for k, v in manifest.items()}
    if manifest["format"] == 1:
        assert embedded == expected
    else:
        extra = {"data_sha256", "data_tar_size", "data_bytes", "pixa_bytes", "app_zlib_size",
                 "app_zlib_sha256", "data_zlib_size", "data_zlib_sha256"}
        assert embedded.keys() == expected.keys() | extra
        assert all(embedded[k] == v for k, v in expected.items()), "manifest identity mismatch"
        for key in ("data_sha256", "app_zlib_sha256", "data_zlib_sha256"):
            _hash(embedded[key])
        for key in ("data_tar_size", "data_bytes", "pixa_bytes", "app_zlib_size", "data_zlib_size"):
            assert re.fullmatch(r"[0-9]+", embedded[key]), ("manifest length", key)
    baseline, stage = snapshot["baseline_status"], snapshot["stage_status"]
    assert baseline["device_uid"] == report["uid"], "wrong baseline device"
    p1 = {k: v for k, v in baseline.items() if k.startswith("partition_1_")}
    assert {"partition_1_" + key for key in P1_FIELDS} <= p1.keys(), "incomplete P1 baseline"
    assert p1["partition_1_valid"] == "1" and p1["partition_1_role"] == "loader"
    assert p1["partition_1_board"] == manifest["board"] and p1["partition_1_target"] == manifest["target"]
    for field in ("image_checksum", "package_checksum"):
        _hash(p1["partition_1_" + field])
    assert int(p1["partition_1_image_size"]) > 0 and int(p1["partition_1_package_size"]) > 0
    assert stage["device_uid"] == report["uid"] and stage["stage_valid"] == "1"
    expected = {"package_checksum": package["sha256"], "image_checksum": image["sha256"],
                "version": report["version"], "board": manifest["board"], "target": manifest["target"],
                "role": "app", "package_size": str(package["size"]), "image_size": str(image["size"])}
    for field, value in expected.items():
        assert stage["stage_" + field] == value, ("staged identity", field)
    runs = snapshot["runs"]
    offset = report.get("status_runs_offset", 1)
    assert offset in (0, 1)
    assert len(runs) == len(report["runs"]) - offset and len(runs) >= 2, "missing final-image boots"
    old = snapshot["baseline_coredump"]
    assert old["result"] == "OK" and old["code"] == "0" and old["blank"] in {"0", "1"}
    stored = int(old["stored_bytes"])
    assert 0 <= stored <= int(old["bytes"])
    assert old["blank"] != "1" or stored == 0
    for run, original in zip(runs, report["runs"][offset:]):
        required = {"final_status", "final_coredump"}
        if old["blank"] == "0":
            required.add("final_coredump_bytes")
        assert run["raw_hashes"].keys() == required, "incomplete boot identity snapshot"
        for field, value in run["raw_hashes"].items():
            _hash(value)
            assert value == original[field]["sha256"], ("boot receipt identity", field)
        final = run["final_status"]
        assert all(final[k] == v for k, v in p1.items()), "P1 changed"
        assert final["device_uid"] == report["uid"] and final["stage_valid"] == "0"
        assert final["running_partition"] == final["next_partition"] == "2" and final["last_result"] == "0"
        assert final["active_role"] == "app" and final["active_version"] == report["version"]
        assert final["active_checksum"] == image["sha256"] and final["active_image_size"] == str(image["size"])
        for field, value in expected.items():
            assert final["partition_2_" + field] == value, ("running identity", field)
        assert run["final_coredump"] == old, "coredump status changed"
        if old["blank"] == "0":
            before, after = snapshot["baseline_coredump_bytes"], run["final_coredump_bytes"]
            _hash(before["sha256"])
            _hash(after["sha256"])
            assert before["size"] == after["size"] == stored, "truncated coredump"
            assert before["sha256"] == after["sha256"], "coredump bytes changed"
    assert snapshot["source_inputs"] == report["source_inputs"], "source receipt changed"
    historical = _historical_source_policy(report)
    assert snapshot.get("historical_source_inputs", {}) == historical, "historical source receipt changed"
    _check_sources(snapshot["source_inputs"], historical)
    return metadata


def _file_identity(ref):
    path = bound_ref(ref)
    return {"sha256": sha(path), "size": path.stat().st_size}


def _audit_snapshot(report):
    """Read actual locally retained artifacts and capture their checked facts."""
    historical = _historical_source_policy(report)
    _verify_historical_git_blobs(historical)
    metadata = json.loads(bound_ref(report["firmware"]).read_text(encoding="utf-8"))
    package = bound_ref(report["package"])
    original = package.read_bytes()
    plain = original if original.startswith(b"manifest\0") else zlib.decompress(original)
    with tarfile.open(fileobj=io.BytesIO(plain), mode="r:") as archive:
        members = archive.getmembers()
        assert len({m.name for m in members}) == len(members), "duplicate archive members"
        manifest_entry = archive.getmember("manifest")
        assert manifest_entry.isfile()
        pairs = [line.split("=", 1) for line in archive.extractfile(manifest_entry).read().decode("ascii").splitlines()]
        assert all(len(pair) == 2 for pair in pairs)
        parsed = dict(pairs)
        assert len(parsed) == len(pairs), "duplicate manifest fields"
        target = metadata["package_manifest"]["target"]
        assert target in {"esp32s3", "bk7258"}
        app_path = "app/esp/app.bin" if target == "esp32s3" else "app/bk/app_ab_crc.rbl"
        if parsed.get("format") == "2":
            assert archive.getnames() == ["manifest", "data.tar.zlib", "app.bin.zlib"]
            for name, prefix in (("data.tar.zlib", "data"), ("app.bin.zlib", "app")):
                member = archive.getmember(name)
                assert member.isfile()
                compressed = archive.extractfile(member).read()
                assert len(compressed) == int(parsed[prefix + "_zlib_size"])
                assert hashlib.sha256(compressed).hexdigest() == parsed[prefix + "_zlib_sha256"]
                if prefix == "app": image = zlib.decompress(compressed)
        else:
            assert parsed.get("format") == "1"
            member = archive.getmember(app_path)
            assert member.isfile()
            image = archive.extractfile(member).read()
    snapshot = {"schema": 1, "capture_method": "verified-local-package-and-loader-bytes",
                "uid": report["uid"], "version": report["version"], "metadata": metadata,
                "package": _file_identity(report["package"]),
                "image": {"sha256": hashlib.sha256(image).hexdigest(), "size": len(image)},
                "archive_manifest": parsed,
                "baseline_status": kv(bound_ref(report["baseline_status"]), "H2_LOADER_STATUS "),
                "stage_status": kv(bound_ref(report["stage_status"]), "H2_LOADER_STATUS "),
                "baseline_coredump": kv(bound_ref(report["baseline_coredump"]), "H2_LOADER_COREDUMP_STATUS "),
                "raw_hashes": {field: report[field]["sha256"] for field in
                               ("firmware", "package", "baseline_status", "stage_status", "baseline_coredump")},
                "source_inputs": report["source_inputs"],
                "historical_source_inputs": historical, "runs": []}
    nonblank = snapshot["baseline_coredump"]["blank"] == "0"
    before = bound_ref(report["baseline_coredump_bytes"]) if nonblank else None
    if nonblank:
        snapshot["baseline_coredump_bytes"] = _file_identity(report["baseline_coredump_bytes"])
        snapshot["raw_hashes"]["baseline_coredump_bytes"] = report["baseline_coredump_bytes"]["sha256"]
    for run in report["runs"][report.get("status_runs_offset", 1):]:
        row = {"final_status": kv(bound_ref(run["final_status"]), "H2_LOADER_STATUS "),
               "final_coredump": kv(bound_ref(run["final_coredump"]), "H2_LOADER_COREDUMP_STATUS "),
               "raw_hashes": {field: run[field]["sha256"] for field in ("final_status", "final_coredump")}}
        if nonblank:
            after = bound_ref(run["final_coredump_bytes"])
            assert before.read_bytes() == after.read_bytes(), "coredump bytes changed"
            row["final_coredump_bytes"] = _file_identity(run["final_coredump_bytes"])
            row["raw_hashes"]["final_coredump_bytes"] = run["final_coredump_bytes"]["sha256"]
        snapshot["runs"].append(row)
    _check_snapshot(report, snapshot)
    return snapshot


def check_status(report):
    """Live audit: requires the actual retained package, logs and raw dumps."""
    snapshot = _audit_snapshot(report)
    if "bindings_snapshot" in report:
        captured = json.loads(bound_ref(report["bindings_snapshot"]).read_text(encoding="utf-8"))
        assert captured == snapshot, "captured evidence differs from local artifact audit"
    return snapshot["metadata"]


def capture_snapshot(report, output):
    """Only generate historical facts after a complete live byte/status audit."""
    snapshot = _audit_snapshot(report)
    path = Path(output)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {"path": str(path), "sha256": sha(path)}


def check_captured_status(report):
    """CI consistency check; does not rerun hardware or access local raw refs."""
    path = Path(report["bindings_snapshot"]["path"])
    assert not path.is_absolute() and ".." not in path.parts, "snapshot must be repository-relative"
    snapshot = json.loads(bound_ref(report["bindings_snapshot"]).read_text(encoding="utf-8"))
    return _check_snapshot(report, snapshot)
