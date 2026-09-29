import argparse
import json
from pathlib import Path
import signal
import threading
import socket
import time
from fixture import Fixture

parser = argparse.ArgumentParser()
parser.add_argument('--bind', default='127.0.0.1')
parser.add_argument('--advertise')
parser.add_argument('--output', required=True)
parser.add_argument('--dns-host', default='ap.e2e.gizclaw.com')
args = parser.parse_args()
root = Path(args.output)
root.mkdir(parents=True, exist_ok=True)
stop = threading.Event()
signal.signal(signal.SIGINT, lambda *_: stop.set())
signal.signal(signal.SIGTERM, lambda *_: stop.set())
with Fixture(args.bind, args.advertise) as fixture:
    (root / 'root.pem').write_bytes(fixture.ca.read_bytes())
    (root / 'wrong.pem').write_bytes(fixture.wrong_ca.read_bytes())
    dns_ip=socket.getaddrinfo(args.dns_host,None,socket.AF_INET,socket.SOCK_STREAM)[0][4][0]
    config = dict(dns_host=args.dns_host, dns_ipv4=dns_ip, host=fixture.advertise, port=fixture.port, session=fixture.session,
        server_name='pal-net-tls.test', epoch_ms=int(time.time()*1000),
        ca_hex=fixture.ca.read_bytes().hex(), wrong_ca_hex=fixture.wrong_ca.read_bytes().hex())
    (root / 'config.json').write_text(json.dumps(config, indent=2)+'\n')
    print(json.dumps({key: value for key, value in config.items() if not key.endswith('_hex')}), flush=True)
    while not stop.wait(1):
        (root / 'peer.json').write_text(json.dumps(fixture.snapshot(), indent=2)+'\n')
    (root / 'peer.json').write_text(json.dumps(fixture.snapshot(), indent=2)+'\n')
