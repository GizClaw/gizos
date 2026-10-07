import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
from fixture_ipv6 import IPv6Fixture
from fixture import Fixture
import contextlib
import re
import select
import time
parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True)
parser.add_argument('--server', required=True)
args = parser.parse_args()
@contextlib.contextmanager
def pion(binary):
    process = subprocess.Popen([str(Path(binary).resolve()), '--listen=[::1]:0',
        '--stun-listen=[::1]:0', '--turn-listen=127.0.0.1:0', '--candidate-ip=::1'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if not select.select([process.stdout], [], [], .1)[0]: continue
            line = process.stdout.readline().decode()
            if not line: raise RuntimeError('IPv6 Pion exited')
            if 'H2_WEBRTC_TEST_SERVER_READY' in line:
                http = re.search(r' http=([^ ]+)', line)[1]
                stun = re.search(r' stun=([^ ]+)', line)[1]
                yield 'http://' + http + '/offer', 'stun:' + stun
                return
        raise TimeoutError('IPv6 Pion startup')
    finally:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
        process.stdout.close()
with IPv6Fixture() as fixture, pion(args.server) as (offer, stun):
    raw = fixture.raw
    dns_host = os.environ.get('H2_PAL_IPV6_DNS_HOST', 'localhost')
    expected = socket.getaddrinfo(dns_host, None, socket.AF_INET6, socket.SOCK_STREAM)[0][4][0]
    run = subprocess.run([str(Path(args.binary).resolve()), raw.advertise, str(raw.port),
        raw.session, str(raw.ca), str(raw.wrong_ca), dns_host, expected,
        fixture.http_url, fixture.fallback_url, str(fixture.mqtt_port), offer, stun, str(fixture.dns_port)],
        capture_output=True, text=True, timeout=600)
    print(run.stdout, end=''); print(run.stderr, end='')
    summaries = [json.loads(line.split(' ', 1)[1]) for line in run.stdout.splitlines()
        if line.startswith('H2_PAL_IPV6_SUMMARY ')]
    assert run.returncode == 0, run.returncode
    assert len(summaries) == 1
    summary = summaries[0]
    assert len(summary['cases']) == 56 and len({c['id'] for c in summary['cases']}) == 56
    assert summary['passed'] == 56 and all(c['status']=='PASS' for c in summary['cases'])
    assert all(summary[key] == 0 for key in ('failed','blocked','rc','teardown',
        'retained_sockets','retained_resolvers','retained_allocations'))
    fixture.verify()
    receipt = dict(summary=summary, peer=fixture.snapshot(), dns=dict(host=dns_host, expected_ipv6=expected),
        artifact_sha256=hashlib.sha256(Path(args.binary).read_bytes()).hexdigest())
    output = os.environ.get('TEST_UNDECLARED_OUTPUTS_DIR')
    if output: Path(output, 'pal-ipv6.json').write_text(json.dumps(receipt, indent=2)+'\n')
