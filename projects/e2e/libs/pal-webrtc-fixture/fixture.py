"""An isolated Pion process with explicit loopback or operator LAN binding."""
import contextlib
import re
import select
import subprocess
import time
from pathlib import Path

@contextlib.contextmanager
def fixture(binary, address='127.0.0.1'):
    endpoint = '[' + address + ']:0' if ':' in address else address + ':0'
    process = subprocess.Popen([str(Path(binary).resolve()), '--listen=' + endpoint,
        '--stun-listen=' + endpoint, '--turn-listen=127.0.0.1:0',
        '--candidate-ip=' + address, '--ice-mode=udp'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if not select.select([process.stdout], [], [], 0.1)[0]: continue
            line = process.stdout.readline().decode()
            if not line: raise RuntimeError('Pion exited before ready')
            if 'H2_WEBRTC_TEST_SERVER_READY' not in line: continue
            http = re.search(r' http=([^ ]+)', line).group(1)
            stun = re.search(r' stun=([^ ]+)', line).group(1)
            yield dict(offer='http://' + http + '/offer', stun='stun:' + stun)
            return
        raise TimeoutError('Pion startup')
    finally:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()
