"""Explicitly serve the same fixture to device Apps; no serial/device actions."""
import argparse
import json
from pathlib import Path
import tempfile
import time
from fixture import Fixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--advertised', default='127.0.0.1')
    parser.add_argument('--bazelrc', required=True, type=Path)
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='h2-http-device-fixture-') as temp, \
            Fixture(temp, bind=args.bind, advertised=args.advertised) as fixture:
        values = dict(HTTP_BASE=fixture.http, HTTPS_BASE=fixture.https,
                      UNTRUSTED_HTTPS_BASE=fixture.untrusted,
                      CA_PEM_HEX=fixture.ca.read_bytes().hex(),
                      FIXTURE_EPOCH_MS=str(int(time.time() * 1000)))
        args.bazelrc.write_text(''.join('build --define=H2_PAL_HTTP_' + key + '=' + value + '\n'
                                       for key, value in values.items()))
        print(json.dumps(dict(status='ready', bazelrc=str(args.bazelrc),
                              http=fixture.http, https=fixture.https,
                              untrusted=fixture.untrusted)), flush=True)
        try:
            while True:
                if args.receipt:
                    runs = {}
                    with fixture.servers[0].lock:
                        names = list(fixture.servers[0].runs)
                    for name in names:
                        try:
                            runs[name] = dict(arrivals=fixture.verify_arrivals(name), arrivals_valid=True, tls_rejection=fixture.verify_tls_rejection(name))
                        except RuntimeError:
                            with fixture.servers[0].lock:
                                runs[name] = dict(arrivals=fixture.servers[0].runs[name].copy(), arrivals_valid=False)
                    args.receipt.write_text(json.dumps(dict(session=fixture.session, runs=runs), indent=2) + '\n')
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
