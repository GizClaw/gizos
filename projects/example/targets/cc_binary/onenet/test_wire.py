"""Run the real OneNET client/CoreMQTT path against a loopback MQTT peer."""

import json
import os
import socket
import subprocess
import sys
import threading
import unittest


def exact(connection, length):
    data = b""
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise EOFError("unexpected MQTT connection close")
        data += chunk
    return data


def packet(connection):
    kind = exact(connection, 1)[0]
    length = 0
    multiplier = 1
    for _ in range(4):
        byte = exact(connection, 1)[0]
        length += (byte & 127) * multiplier
        if byte < 128:
            return kind, exact(connection, length)
        multiplier *= 128
    raise ValueError("invalid MQTT remaining length")


def text(data):
    length = int.from_bytes(data[:2], "big")
    return data[2:2 + length].decode(), data[2 + length:]


def publish(connection, topic, payload):
    encoded_topic = topic.encode()
    body = len(encoded_topic).to_bytes(2, "big") + encoded_topic
    body += json.dumps(payload, separators=(",", ":")).encode()
    length = len(body)
    header = bytearray([0x30])
    while True:
        byte = length % 128
        length //= 128
        header.append(byte | (128 if length else 0))
        if not length:
            break
    connection.sendall(header + body)


class WireTest(unittest.TestCase):
    def test_explicit_tls_identity_requires_explicit_ca(self):
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith("ONENET_")}
        environment.update(ONENET_PARAMS_JSON="{}", ONENET_ACCESS_TOKEN="fixture-token",
                           ONENET_TLS_SERVER_NAME="fixture-identity")
        result = subprocess.run([BINARY, "127.0.0.1", "1", "p", "n"],
                                env=environment, capture_output=True, text=True,
                                timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn("requires ONENET_CA_FILE", result.stderr)

    def run_negative_exchange(self, exchange):
        failures = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(20)

            def broker():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(15)
                        exchange(connection)
                except BaseException as error:
                    failures.append(error)

            worker = threading.Thread(target=broker, daemon=True)
            worker.start()
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("ONENET_")}
            environment.update(
                ONENET_PLAINTEXT="1", ONENET_ACCESS_TOKEN="test-token",
                ONENET_PARAMS_JSON='{"battery":{"value":42}}',
            )
            result = subprocess.run(
                [BINARY, "127.0.0.1", str(listener.getsockname()[1]),
                 "product-1", "device-1"],
                env=environment, capture_output=True, text=True, timeout=20,
            )
            worker.join(timeout=20)
            self.assertFalse(worker.is_alive(), "loopback broker did not finish")
            if failures:
                raise failures[0]
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("ONENET finished result=", result.stdout)
            return result

    def test_refused_authentication_does_not_subscribe_or_post(self):
        def refused(connection):
            self.assertEqual(packet(connection)[0], 0x10)
            connection.sendall(b"\x20\x02\x00\x05")
            self.assertEqual(connection.recv(1), b"")

        result = self.run_negative_exchange(refused)
        self.assertNotIn("ONENET response", result.stdout)

    def test_platform_rejection_and_missing_reply_fail_and_disconnect(self):
        for reply in (403, None):
            with self.subTest(reply=reply):
                def rejected(connection):
                    self.assertEqual(packet(connection)[0], 0x10)
                    connection.sendall(b"\x20\x02\x00\x00")
                    kind, body = packet(connection)
                    self.assertEqual(kind, 0x82)
                    connection.sendall(b"\x90\x03" + body[:2] + b"\x01")
                    kind, body = packet(connection)
                    self.assertEqual(kind, 0x30)
                    topic, body = text(body)
                    self.assertEqual(topic, "$sys/product-1/device-1/thing/property/post")
                    if reply is not None:
                        publish(connection, topic + "/reply",
                                {"id": json.loads(body)["id"], "code": reply})
                    self.assertEqual(packet(connection), (0xE0, b""))

                result = self.run_negative_exchange(rejected)
                if reply is not None:
                    self.assertIn("code=403", result.stdout)
                else:
                    self.assertIn("ONENET finished result=-6", result.stdout)

    def test_connect_publish_command_and_close(self):
        failures = []
        prefix = "$sys/product-1/device-1/"
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(20)

            def broker():
                try:
                    connection, _ = listener.accept()
                    with connection:
                        connection.settimeout(15)
                        kind, body = packet(connection)
                        self.assertEqual(kind, 0x10)
                        protocol, rest = text(body)
                        self.assertEqual(protocol, "MQTT")
                        self.assertEqual(rest[0], 4)
                        self.assertEqual(rest[1], 0xC2)
                        device, rest = text(rest[4:])
                        product, rest = text(rest)
                        credential, rest = text(rest)
                        self.assertEqual((device, product, credential, rest),
                                         ("device-1", "product-1", "test-token", b""))
                        connection.sendall(b"\x20\x02\x00\x00")
                        kind, body = packet(connection)
                        self.assertEqual(kind, 0x82)
                        topic, qos = text(body[2:])
                        self.assertEqual(topic, prefix + "thing/#")
                        self.assertEqual(qos, b"\x01")
                        connection.sendall(b"\x90\x03" + body[:2] + b"\x01")
                        kind, body = packet(connection)
                        self.assertEqual(kind, 0x30)
                        topic, body = text(body)
                        self.assertEqual(topic, prefix + "thing/property/post")
                        document = json.loads(body)
                        self.assertEqual(document["params"],
                                         {"battery": {"value": 42},
                                          "enabled": {"value": True}})
                        publish(connection, prefix + "thing/property/set",
                                {"id": "007", "version": "1.0",
                                 "params": {"enabled": False}})
                        publish(connection, prefix + "thing/property/post/reply",
                                {"id": document["id"], "code": 200})
                        kind, body = packet(connection)
                        self.assertEqual(kind, 0x30)
                        topic, body = text(body)
                        self.assertEqual(topic, prefix + "thing/property/set_reply")
                        self.assertEqual(json.loads(body)["id"], "007")
                        self.assertEqual(json.loads(body)["code"], 501)
                        self.assertEqual(packet(connection), (0xE0, b""))
                except BaseException as error:
                    failures.append(error)

            worker = threading.Thread(target=broker, daemon=True)
            worker.start()
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("ONENET_")}
            environment.update(
                ONENET_PLAINTEXT="1",
                ONENET_ACCESS_TOKEN="test-token",
                ONENET_PARAMS_JSON='{"battery":{"value":42},"enabled":{"value":true}}',
            )
            result = subprocess.run(
                [BINARY, "127.0.0.1", str(listener.getsockname()[1]),
                 "product-1", "device-1"],
                env=environment, capture_output=True, text=True, timeout=20,
            )
            worker.join(timeout=20)
            self.assertFalse(worker.is_alive(), "loopback broker did not finish")
            if failures:
                raise failures[0]
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ONENET response result=0 code=200", result.stdout)
            self.assertIn("ONENET command operation=5 reply=0", result.stdout)
            self.assertIn("ONENET finished result=0", result.stdout)


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1))
    unittest.main()
