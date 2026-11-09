import argparse
import json
from pathlib import Path
import signal
import threading
import socket
import time
from datetime import datetime, timezone
import ipaddress
from fixture import Fixture

parser = argparse.ArgumentParser()
parser.add_argument('--bind', default='127.0.0.1')
parser.add_argument('--advertise')
parser.add_argument('--output', required=True)
parser.add_argument('--dns-host', default='ap.e2e.gizclaw.com')
parser.add_argument('--dns-expectation', type=Path,
    help='Independent operator DNS observation JSON for the DUT resolver view')
args = parser.parse_args()
root = Path(args.output)
root.mkdir(parents=True, exist_ok=True)
stop = threading.Event()
signal.signal(signal.SIGINT, lambda *_: stop.set())
signal.signal(signal.SIGTERM, lambda *_: stop.set())
with Fixture(args.bind, args.advertise) as fixture:
    (root / 'root.pem').write_bytes(fixture.ca.read_bytes())
    (root / 'wrong.pem').write_bytes(fixture.wrong_ca.read_bytes())
    if args.dns_expectation:
        observation=json.loads(args.dns_expectation.read_text())
        assert observation['hostname']==args.dns_host
        assert observation['resolver'] and observation['method']
        assert datetime.fromisoformat(observation['observed_at_utc']).utcoffset().total_seconds()==0
        dns_ip=str(ipaddress.IPv4Address(observation['ipv4']))
    else:
        dns_ip=socket.getaddrinfo(args.dns_host,None,socket.AF_INET,socket.SOCK_STREAM)[0][4][0]
        observation=dict(hostname=args.dns_host,ipv4=dns_ip,resolver='operator system resolver',
            method='host socket.getaddrinfo independent of the PAL',
            observed_at_utc=datetime.now(timezone.utc).isoformat())
    config = dict(dns_host=args.dns_host, dns_ipv4=dns_ip, host=fixture.advertise, port=fixture.port, session=fixture.session,
        server_name='pal-net-tls.test', epoch_ms=int(time.time()*1000),
        ca_hex=fixture.ca.read_bytes().hex(), wrong_ca_hex=fixture.wrong_ca.read_bytes().hex(),
        dns_observation=observation)
    (root / 'config.json').write_text(json.dumps(config, indent=2)+'\n')
    print(json.dumps({key: value for key, value in config.items() if not key.endswith('_hex')}), flush=True)
    while not stop.wait(1):
        (root / 'peer.json').write_text(json.dumps(fixture.snapshot(), indent=2)+'\n')
    (root / 'peer.json').write_text(json.dumps(fixture.snapshot(), indent=2)+'\n')
