"""Run the production client over CoreHTTP and the host Net PAL on loopback."""

import contextlib
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())
LIVE_BINARY = str(Path(sys.argv.pop(1)).resolve())
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
           oversized=False, reject_heartbeat=False):
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
            if reject_heartbeat and self.path.endswith("/report"):
                response = {"code": "403", "success": False}
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
    def invoke(self, origin, *, allow_http=True, params=PARAMS, overrides=None, binary=BINARY):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("LINGXI_")}
        env.update(LINGXI_PRODUCT_ID="fixture-product", LINGXI_PRODUCT_KEY=PRODUCT_KEY,
                   LINGXI_SOURCE_IP="192.0.2.5", LINGXI_BASE_URL=origin,
                   LINGXI_DEVICE_NO_TYPE="SN", LINGXI_DEVICE_NO="registered-fixture",
                   LINGXI_HEARTBEAT_JSON=json.dumps(params))
        if allow_http:
            env["LINGXI_LOOPBACK_HTTP"] = "1"
        for name, value in (overrides or {}).items():
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
        result = subprocess.run([binary], env=env, text=True, capture_output=True,
                                timeout=15, check=False)
        self.assertNotIn(PRODUCT_KEY, result.stdout + result.stderr)
        self.assertNotIn(DEVICE_SECRET, result.stdout + result.stderr)
        return result

    def test_get_info_then_single_heartbeat(self):
        with server() as (origin, requests):
            result = self.invoke(origin)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
                if line.startswith("H2_LINGXI_E2E_CASE ")]
        self.assertEqual([row["id"] for row in rows],
                         ["get-device-info", "heartbeat", "credential-cleanup"])
        self.assertEqual([row["status"] for row in rows], ["PASS"] * 3)
        for row in rows[:2]:
            self.assertEqual((row["http"], row["code"], row["success"]), (200, 200, True))
        summary = json.loads(next(line.split(" ", 1)[1] for line in result.stdout.splitlines()
                                 if line.startswith("H2_LINGXI_E2E_SUMMARY ")))
        self.assertEqual(summary, {"passed": 3, "failed": 0, "blocked": 0, "result": 0, "cleanup": 0})
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

    def test_numeric_code(self):
        with server(code=200) as (origin, requests):
            result = self.invoke(origin)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(requests), 2)

    def test_rejections_stop_without_heartbeat_or_retry(self):
        cases = ({"status": 401}, {"code": "403", "success": False},
                 {"mismatch": True}, {"oversized": True})
        for case in cases:
            with self.subTest(case=case), server(**case) as (origin, requests):
                result = self.invoke(origin)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(len(requests), 1)
                rows = [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
                        if line.startswith("H2_LINGXI_E2E_CASE ")]
                self.assertEqual([row["status"] for row in rows], ["FAIL", "BLOCKED", "PASS"])

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

    def test_invalid_params_environment_stops_before_network(self):
        with server() as (origin, requests):
            result = self.invoke(origin, params={"innerIp": "incorrect-type"})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(requests, [])

    def test_heartbeat_rejection_preserves_cleanup_and_failure(self):
        with server(reject_heartbeat=True) as (origin, requests):
            result = self.invoke(origin)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(requests), 2)
        rows = [json.loads(line.split(" ", 1)[1]) for line in result.stdout.splitlines()
                if line.startswith("H2_LINGXI_E2E_CASE ")]
        self.assertEqual([row["status"] for row in rows], ["PASS", "FAIL", "PASS"])

    def test_live_entry_rejects_plaintext_fixture(self):
        for allow_http in (False, True):
            with self.subTest(allow_http=allow_http), server() as (origin, requests):
                result = self.invoke(origin, binary=LIVE_BINARY, allow_http=allow_http)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(requests, [])

    def test_missing_or_malformed_environment_stops_before_network(self):
        for overrides in ({"LINGXI_PRODUCT_KEY": None}, {"LINGXI_HEARTBEAT_JSON": None},
                          {"LINGXI_DEVICE_NO_TYPE": "unknown"}, {"LINGXI_TIMEOUT_MS": "0"},
                          {"LINGXI_HEARTBEAT_JSON": "{"}):
            with self.subTest(overrides=overrides), server() as (origin, requests):
                result = self.invoke(origin, overrides=overrides)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(requests, [])


if __name__ == "__main__":
    unittest.main()
