"""Separate live artifact audits from captured qualification consistency checks."""
import hashlib
import io
import json
from pathlib import Path
import re
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


def _check_sources(inputs):
    assert inputs, "missing source snapshot"
    for path, expected in inputs.items():
        _hash(expected)
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
    assert manifest["format"] == 1 and manifest["role"] == "app"
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
    assert snapshot["archive_manifest"] == {k: str(v) for k, v in manifest.items()}
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
    _check_sources(snapshot["source_inputs"])
    return metadata


def _file_identity(ref):
    path = bound_ref(ref)
    return {"sha256": sha(path), "size": path.stat().st_size}


def _audit_snapshot(report):
    """Read actual locally retained artifacts and capture their checked facts."""
    metadata = json.loads(bound_ref(report["firmware"]).read_text(encoding="utf-8"))
    package = bound_ref(report["package"])
    with tarfile.open(fileobj=io.BytesIO(zlib.decompress(package.read_bytes())), mode="r:") as archive:
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
                "source_inputs": report["source_inputs"], "runs": []}
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
