"""Inject process environment into an owner-only managed carrier package."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import time

from projects.h2loader.tools.bazel.firmware_artifacts import BundleEntry, write_package

LINGXI_FIELDS = {
    "base_url": "LINGXI_BASE_URL",
    "product_id": "LINGXI_PRODUCT_ID",
    "product_key": "LINGXI_PRODUCT_KEY",
    "device_no_type": "LINGXI_DEVICE_NO_TYPE",
    "device_no": "LINGXI_DEVICE_NO",
}
ONENET_FIELDS = {
    "host": "ONENET_HOST",
    "port": "ONENET_PORT",
    "product_id": "ONENET_PRODUCT_ID",
    "device_name": "ONENET_DEVICE_NAME",
    "params_json": "ONENET_PARAMS_JSON",
}


def configuration(environment, epoch_ms):
    """Select explicitly configured platforms; never share their credentials."""
    result = {"epoch_ms": epoch_ms}
    for platform, fields in (("lingxi", LINGXI_FIELDS), ("onenet", ONENET_FIELDS)):
        names = list(fields.values())
        if platform == "onenet":
            names += ["ONENET_ACCESS_TOKEN", "ONENET_DEVICE_KEY",
                      "ONENET_CA_FILE", "ONENET_TLS_SERVER_NAME"]
        if not any(name in environment for name in names):
            continue
        missing = [name for name in fields.values() if not environment.get(name)]
        if missing:
            raise ValueError("Missing environment: " + ", ".join(missing))
        values = {field: environment[name] for field, name in fields.items()}
        if platform == "onenet":
            port = values["port"]
            if not port.isascii() or not port.isdecimal() or not 0 < int(port) < 65536:
                raise ValueError("Invalid ONENET_PORT")
            values["port"] = int(port)
            token = environment.get("ONENET_ACCESS_TOKEN")
            key = environment.get("ONENET_DEVICE_KEY")
            if bool(token) == bool(key):
                raise ValueError("Set exactly one of ONENET_ACCESS_TOKEN / ONENET_DEVICE_KEY")
            values["access_token" if token else "device_key"] = token or key
            try:
                params = json.loads(values["params_json"])
            except ValueError:
                raise ValueError("Invalid ONENET_PARAMS_JSON") from None
            if not isinstance(params, dict):
                raise ValueError("ONENET_PARAMS_JSON must be an object")
            ca_path = environment.get("ONENET_CA_FILE")
            expected_name = environment.get("ONENET_TLS_SERVER_NAME")
            if expected_name and not ca_path:
                raise ValueError("ONENET_TLS_SERVER_NAME requires ONENET_CA_FILE")
            if ca_path:
                with Path(ca_path).open("rb") as file:
                    ca = file.read(8192)
                if (len(ca) >= 8192 or b"PRIVATE KEY" in ca or
                        b"-----BEGIN CERTIFICATE-----" not in ca):
                    raise ValueError("Invalid ONENET_CA_FILE")
                values["ca_pem"] = ca.decode("ascii")
            if expected_name:
                values["tls_server_name"] = expected_name
        result[platform] = values
    if len(result) == 1:
        raise ValueError("Configure LINGXI_* and/or ONENET_* environment")
    return result


def provision(image_path, metadata_path, output_path, environment):
    """Reuse the built image; package secrets only after the Bazel action."""
    if os.name != "posix":
        raise ValueError("Owner-only provisioning requires a POSIX host")
    metadata = json.loads(metadata_path.read_text())
    manifest = metadata["package_manifest"]
    if metadata["entry"] != "projects/e2e/targets/h2loader_tar_zlib/carrier/devkit":
        raise ValueError("Expected the carrier E2E artifact")
    if (manifest["role"], manifest["board"], manifest["target"]) != (
        "app", "devkit", "esp32s3"
    ):
        raise ValueError("Expected a DevKit ESP32-S3 App artifact")
    image = image_path.read_bytes()
    if len(image) != manifest["image_size"] or (
        hashlib.sha256(image).hexdigest() != manifest["image_sha256"]
    ):
        raise ValueError("Image does not match firmware metadata")
    config = configuration(environment, int(time.time() * 1000))
    payload = json.dumps(config, separators=(",", ":")).encode()
    if len(payload) > 16384:
        raise ValueError("Runtime configuration exceeds the device input budget")
    # Refuse overwrites and make the secret-bearing file private from creation.
    output_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(output_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(descriptor)
    try:
        write_package(
            output_path, "app/esp/app.bin", image,
            [BundleEntry("data/carrier-e2e.json", payload)],
            role="app", board="devkit", target="esp32s3",
            version=manifest["version"], package_format=1,
        )
    except BaseException:
        output_path.unlink(missing_ok=True)
        raise


def main():
    environment_help = (
        "Configure either platform or both; incomplete selected configurations fail.\n"
        "Lingxi required: " + ", ".join(LINGXI_FIELDS.values()) + "\n"
        "OneNET required: " + ", ".join(ONENET_FIELDS.values()) + "\n"
        "OneNET credential: exactly one of ONENET_DEVICE_KEY / ONENET_ACCESS_TOKEN.\n"
        "OneNET optional: ONENET_CA_FILE and ONENET_TLS_SERVER_NAME (requires CA).\n"
        "Wi-Fi and heartbeat metadata are read from the device Runtime."
    )
    parser = argparse.ArgumentParser(
        description=__doc__, epilog=environment_help,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--app-bin", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        provision(args.app_bin, args.metadata, args.output, os.environ)
    except (OSError, ValueError, KeyError):
        # Do not echo configuration, paths, source JSON or secret values.
        parser.exit(2, "Carrier provisioning failed; check inputs and required environment.\n")
    print("Carrier runtime package prepared (owner-only; no credentials in build inputs).")


if __name__ == "__main__":
    main()
