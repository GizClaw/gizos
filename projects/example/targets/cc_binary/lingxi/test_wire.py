"""Run the production client over CoreHTTP and the host Net PAL on loopback."""

import contextlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
PRODUCT_KEY = 'fixture-"key\\with\nnew-line'
DEVICE_SECRET = "fixture-device-secret"
PARAMS = {
    "innerIp": ["192.0.2.3", "::1"],
    "netSpeed": "100Mbps",
    "netType": "wifi",
    "platform": "fixture-platform",
    "sdkVersion": "ai_fixture",
    "firmwareVersion": "fixture-firmware",
    "imei": "",
    "cmei": "fixture-cmei",
    "mac": "",
}


@contextlib.contextmanager
def server(*, code="200", success=True, status=200, mismatch=False, redirect=None,
           oversized=False):
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            requests.append((self.path, dict(self.headers), json.loads(body)))
            if redirect:
                self.send_response(302)
                self.send_header("Location", redirect)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            response = {"code": code, "success": success}
            if self.path.endswith("/secret/info"):
                response["data"] = {
                    "productId": "fixture-product",
                    "deviceNo": "wrong-device" if mismatch else "registered-fixture",
                    "deviceId": "fixture-device",
                    "deviceSecret": DEVICE_SECRET,
                }
            payload = b"x" * 17000 if oversized else json.dumps(response).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_args):
            pass

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{httpd.server_port}", requests
    finally:
        httpd.shutdown()
        httpd.server_close()
        thread.join(timeout=3)


class WireTest(unittest.TestCase):
    def invoke(self, origin, *, heartbeat=True, allow_http=True, params=PARAMS):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("LINGXI_")}
        env.update(LINGXI_PRODUCT_ID="fixture-product", LINGXI_PRODUCT_KEY=PRODUCT_KEY,
                   LINGXI_SOURCE_IP="192.0.2.5")
        if allow_http:
            env["LINGXI_LOOPBACK_HTTP"] = "1"
        with tempfile.TemporaryDirectory() as directory:
            command = [BINARY, origin, "SN", "registered-fixture"]
            if heartbeat:
                path = Path(directory) / "params.json"
                path.write_text(json.dumps(params))
                command.append(str(path))
            result = subprocess.run(command, env=env, text=True, capture_output=True,
                                    timeout=15, check=False)
        self.assertNotIn(PRODUCT_KEY, result.stdout + result.stderr)
        self.assertNotIn(DEVICE_SECRET, result.stdout + result.stderr)
        return result

    def test_get_info_then_single_heartbeat(self):
        with server() as (origin, requests):
            result = self.invoke(origin)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("LINGXI get-device-info result=0 http=200 code=200 success=1", result.stdout)
        self.assertIn("LINGXI heartbeat result=0 http=200 code=200 success=1", result.stdout)
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0][0], "/v2/customer/device/secret/info")
        self.assertEqual(requests[0][2], {
            "productId": "fixture-product", "productKey": PRODUCT_KEY,
            "deviceNoType": "SN", "deviceNo": "registered-fixture",
        })
        self.assertEqual(requests[1][0], "/v2/customer/device/report")
        self.assertEqual(requests[1][2], {
            "productId": "fixture-product", "productKey": PRODUCT_KEY,
            "deviceId": "fixture-device", "deviceSecret": DEVICE_SECRET,
            "params": PARAMS,
        })
        for _, headers, _ in requests:
            self.assertEqual(headers["Content-Type"], "application/json")
            self.assertEqual(headers["X-AI-IP"], "192.0.2.5")

    def test_query_only_and_numeric_code(self):
        with server(code=200) as (origin, requests):
            result = self.invoke(origin, heartbeat=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(requests), 1)
        self.assertNotIn("LINGXI heartbeat", result.stdout)

    def test_rejections_stop_without_heartbeat_or_retry(self):
        cases = ({"status": 401}, {"code": "403", "success": False},
                 {"mismatch": True}, {"oversized": True})
        for case in cases:
            with self.subTest(case=case), server(**case) as (origin, requests):
                result = self.invoke(origin)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(len(requests), 1)
                self.assertNotIn("LINGXI heartbeat", result.stdout)

    def test_redirect_does_not_forward_credentials(self):
        with server() as (trap, trapped), server(redirect=trap) as (origin, requests):
            result = self.invoke(origin)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(requests), 1)
        self.assertEqual(trapped, [])

    def test_plaintext_requires_explicit_loopback_opt_in(self):
        with server() as (origin, requests):
            result = self.invoke(origin, allow_http=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(requests, [])

    def test_invalid_params_file_stops_before_network(self):
        with server() as (origin, requests):
            result = self.invoke(origin, params={"innerIp": "incorrect-type"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(requests, [])


if __name__ == "__main__":
    unittest.main()
