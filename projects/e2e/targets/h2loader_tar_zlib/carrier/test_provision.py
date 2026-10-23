"""Check runtime-only credential isolation and private package creation."""

import hashlib
import io
import json
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
import zlib

from projects.e2e.targets.h2loader_tar_zlib.carrier.provision import configuration, provision


class ProvisionTest(unittest.TestCase):
    def setUp(self):
        self.environment = {
            "ONENET_HOST": "fixture.example",
            "ONENET_PORT": "8883",
            "ONENET_PRODUCT_ID": "product-fixture",
            "ONENET_DEVICE_NAME": "device-fixture",
            "ONENET_DEVICE_KEY": "Zml4dHVyZQ==",
            "ONENET_PARAMS_JSON": '{"battery":{"value":42}}',
        }

    def test_no_implicit_platform_or_credential_reuse(self):
        value = configuration(self.environment, 123)
        self.assertNotIn("lingxi", value)
        self.assertNotIn("access_token", value["onenet"])
        self.assertEqual(value["onenet"]["port"], 8883)
        self.environment["LINGXI_PRODUCT_KEY"] = "different-fixture-key"
        with self.assertRaises(ValueError):
            configuration(self.environment, 123)
        del self.environment["LINGXI_PRODUCT_KEY"]
        self.environment["ONENET_ACCESS_TOKEN"] = "fixture-token"
        with self.assertRaises(ValueError):
            configuration(self.environment, 123)

    def test_private_package_preserves_built_image_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "app.bin"
            image.write_bytes(b"fixture-image")
            metadata = root / "firmware.json"
            metadata.write_text(json.dumps({
                "entry": "projects/e2e/targets/h2loader_tar_zlib/carrier/devkit",
                "package_manifest": {
                    "role": "app", "board": "devkit", "target": "esp32s3",
                    "version": "fixture-version", "image_size": image.stat().st_size,
                    "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
                },
            }))
            output = root / "private.update.tar.zlib"
            provision(image, metadata, output, self.environment)
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            with tarfile.open(fileobj=io.BytesIO(zlib.decompress(output.read_bytes()))) as archive:
                self.assertEqual(archive.extractfile("app/esp/app.bin").read(), image.read_bytes())
                data = json.load(archive.extractfile("data/carrier-e2e.json"))
                self.assertNotIn("lingxi", data)
                self.assertEqual(data["onenet"]["device_key"], self.environment["ONENET_DEVICE_KEY"])
            before = output.read_bytes()
            with self.assertRaises(FileExistsError):
                provision(image, metadata, output, self.environment)
            self.assertEqual(output.read_bytes(), before)
            image.write_bytes(b"changed-image")
            with self.assertRaises(ValueError):
                provision(image, metadata, root / "mismatch.update.tar.zlib", self.environment)

    def test_custom_identity_requires_explicit_ca_and_rejects_private_key(self):
        self.environment["ONENET_TLS_SERVER_NAME"] = "fixture-identity"
        with self.assertRaises(ValueError):
            configuration(self.environment, 123)
        with tempfile.TemporaryDirectory() as directory:
            ca = Path(directory) / "ca.pem"
            ca.write_text("-----BEGIN CERTIFICATE-----\nfixture\n-----END CERTIFICATE-----\n")
            self.environment["ONENET_CA_FILE"] = str(ca)
            value = configuration(self.environment, 123)["onenet"]
            self.assertEqual(value["tls_server_name"], "fixture-identity")
            self.assertEqual(value["ca_pem"], ca.read_text())
            ca.write_text("-----BEGIN PRIVATE KEY-----\nfixture\n-----END PRIVATE KEY-----\n")
            with self.assertRaises(ValueError):
                configuration(self.environment, 123)


if __name__ == "__main__":
    unittest.main()
