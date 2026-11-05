"""Runtime key descriptor and process-protection contracts for firmware tools."""

import os
import sys
import unittest


@unittest.skipIf(os.name == "nt", "runtime secret descriptors require POSIX")
class FirmwareCryptoTest(unittest.TestCase):
    def test_linux_dump_protection_fails_closed(self):
        from tools.bazel.firmware_crypto import protect_process
        from unittest.mock import patch

        with patch("sys.platform", "linux"), patch("resource.setrlimit"), patch("ctypes.CDLL") as library:
            for results in ((-1,), (0, 1)):
                library.return_value.prctl.side_effect = results
                with self.subTest(results=results), self.assertRaisesRegex(ValueError, "DUMP_PROTECTION_FAILED"):
                    protect_process()
            library.return_value.prctl.side_effect = (0, 0)
            protect_process()

    @unittest.skipUnless(sys.platform == "linux", "Linux process dumpability")
    def test_linux_process_is_not_dumpable(self):
        import subprocess
        import sys

        script = (
            "import sys; sys.path[:] = " + repr(sys.path) + "; "
            "from tools.bazel.firmware_crypto import protect_process; "
            "protect_process(); import ctypes; "
            "assert ctypes.CDLL(None).prctl(3, 0, 0, 0, 0) == 0"
        )
        subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, timeout=10)

    def test_regular_key_files_are_rejected(self):
        import tempfile
        from tools.bazel.firmware_crypto import read_key

        with tempfile.TemporaryFile() as stream:
            stream.write(bytes(range(32)))
            stream.seek(0)
            with self.assertRaisesRegex(ValueError, "KEY_REQUIRES_PIPE_OR_SOCKET"):
                read_key(stream.fileno())

    def test_secret_stream_has_a_total_deadline(self):
        from tools.bazel.firmware_crypto import read_key
        import socket

        for data in (b"", bytes(16), bytes(32)):
            with self.subTest(length=len(data)):
                reader, writer = os.pipe()
                try:
                    os.write(writer, data)
                    with self.assertRaisesRegex(ValueError,"KEY_INPUT_TIMEOUT"):
                        read_key(reader, timeout=0.02)
                finally:
                    os.close(reader)
                    os.close(writer)
        # select.select rejects valid descriptors >= FD_SETSIZE on POSIX.
        # Exercise the public descriptor contract when the host limit allows it.
        import fcntl
        import resource
        if resource.getrlimit(resource.RLIMIT_NOFILE)[0] > 1024:
            reader, writer = os.pipe()
            high_fd = None
            try:
                high_fd = fcntl.fcntl(reader, fcntl.F_DUPFD, 1024)
                os.write(writer,bytes(range(32)))
                os.close(writer)
                writer = None
                self.assertEqual(read_key(high_fd),bytearray(range(32)))
            finally:
                os.close(reader)
                if high_fd is not None:
                    os.close(high_fd)
                if writer is not None:
                    os.close(writer)
        left, right = socket.socketpair()
        try:
            right.sendall(bytes(range(32)))
            right.shutdown(socket.SHUT_WR)
            self.assertEqual(read_key(left.fileno()),bytearray(range(32)))
        finally:
            left.close()
            right.close()


if __name__ == "__main__":
    unittest.main()
