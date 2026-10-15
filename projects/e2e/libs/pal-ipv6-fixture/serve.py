"""Keep the controlled IPv6 peers alive and emit public DevKit build inputs."""
import argparse
import ipaddress
import json
from pathlib import Path
import signal
import threading
import time
from fixture_ipv6 import IPv6Fixture, pion

parser = argparse.ArgumentParser()
parser.add_argument('--server', required=True)
parser.add_argument('--bind', default='::1')
parser.add_argument('--advertise', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--dns-host', default='localhost')
parser.add_argument('--dns-ipv6', default='::1')
args = parser.parse_args()
address = ipaddress.IPv6Address(args.advertise)
if address.is_link_local:
    parser.error('the complete ICE matrix requires a ULA/global IPv6 peer; link-local scope is assessed separately')
args.output.mkdir(parents=True, exist_ok=True)
stop = threading.Event()
signal.signal(signal.SIGINT, lambda *_: stop.set())
signal.signal(signal.SIGTERM, lambda *_: stop.set())
with IPv6Fixture(args.bind, args.advertise) as fixture, pion(args.server, args.bind, args.advertise) as rtc:
    raw = fixture.raw
    config = dict(H2_PAL_IPV6_HOST=args.advertise, H2_PAL_IPV6_PORT=raw.port,
        H2_PAL_IPV6_SESSION=raw.session, H2_PAL_IPV6_CA_HEX=raw.ca.read_bytes().hex(),
        H2_PAL_IPV6_WRONG_CA_HEX=raw.wrong_ca.read_bytes().hex(),
        H2_PAL_IPV6_DNS_HOST=args.dns_host, H2_PAL_IPV6_DNS_IPV6=args.dns_ipv6,
        H2_PAL_IPV6_HTTP_URL=fixture.http_url, H2_PAL_IPV6_FALLBACK_URL='',
        H2_PAL_IPV6_MQTT_PORT=fixture.mqtt_port, H2_PAL_IPV6_DNS_PORT=fixture.dns_port,
        H2_PAL_IPV6_OFFER_URL=rtc['offer'], H2_PAL_IPV6_STUN_URL=rtc['stun'],
        H2_PAL_IPV6_EPOCH_MS=int(time.time() * 1000))
    (args.output / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
    (args.output / 'fixture.bazelrc').write_text(''.join(
        'build --define=' + key + '=' + str(value) + '\n' for key, value in config.items()))
    print('H2_PAL_IPV6_FIXTURE_READY ' + json.dumps(dict(session=raw.session,
        host=args.advertise, control_port=raw.port, dns_port=fixture.dns_port,
        config=str(args.output / 'config.json'))), flush=True)
    try:
        while not stop.wait(.5):
            (args.output / 'peer.json').write_text(json.dumps(fixture.snapshot(), indent=2) + '\n')
    finally:
        (args.output / 'peer.json').write_text(json.dumps(fixture.snapshot(), indent=2) + '\n')
