from pathlib import Path
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from tone_fixture import PATH, byte_range, server


class Tone(unittest.TestCase):
    def test_actual_range_head_and_unknown_route(self):
        payload = b"OggS" + bytes(range(256))
        with tempfile.TemporaryDirectory() as temporary:
            file = Path(temporary) / "tone.ogg"
            file.write_bytes(payload)
            with server(file) as instance:
                worker = threading.Thread(target=instance.serve_forever, daemon=True)
                worker.start()
                uri = f"http://127.0.0.1:{instance.server_port}{PATH}"
                with urlopen(Request(uri, headers={"Range": "bytes=4-9"})) as response:
                    self.assertEqual(response.status, 206)
                    self.assertEqual(response.headers["Content-Range"], f"bytes 4-9/{len(payload)}")
                    self.assertEqual(response.read(), payload[4:10])
                with urlopen(Request(uri, method="HEAD")) as response:
                    self.assertEqual(int(response.headers["Content-Length"]), len(payload))
                    self.assertEqual(response.read(), b"")
                with self.assertRaises(HTTPError) as failure:
                    urlopen(Request(uri, headers={"Range": "bytes=99999-"}))
                self.assertEqual(failure.exception.code, 416)
                with self.assertRaises(HTTPError) as failure:
                    urlopen(uri + "/../device")
                self.assertEqual(failure.exception.code, 404)
                instance.shutdown()
                worker.join()
        self.assertEqual(byte_range("bytes=-5", 10), (5, 9, 206))
        with self.assertRaises(ValueError):
            byte_range("bytes=0-1,4-5", 10)


if __name__ == "__main__":
    unittest.main()
