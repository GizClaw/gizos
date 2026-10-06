"""Observe exact case ledger, real MQTT/TLS wire witnesses and cleanup."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from mqtt_fixture import Fixture


def validate(output, registry, smoke=False):
    expected = re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)', Path(registry).read_text())
    if smoke:
        expected = ['publish-qos0']
    rows = [json.loads(line.split(' ', 1)[1]) for line in output.splitlines()
            if line.startswith('H2_PAL_MQTT_CASE ')]
    summaries = [json.loads(line.split(' ', 1)[1]) for line in output.splitlines()
                 if line.startswith('H2_PAL_MQTT_SUMMARY ')]
    if [row['id'] for row in rows] != expected or any(row['status'] != 'PASS' for row in rows):
        raise RuntimeError('MQTT missing, duplicate, out-of-order or failing case')
    if len(summaries) != 1 or summaries[0] != dict(selected=len(expected), passed=len(expected), failed=0,
                                                blocked=0, retained_allocations=0, invalid_frees=0, cleanup=0):
        raise RuntimeError('MQTT summary or final cleanup failed')
    if any(row['live_allocations'] != rows[0]['live_allocations'] or row['invalid_frees'] != 0 for row in rows):
        raise RuntimeError('MQTT case retained a provider allocation')
    if not smoke:
        retained = next(row for row in rows if row['id'] == 'retained-delivery')
        if retained.get('received') != 2:
            raise RuntimeError('retained clear did not observe the actual empty payload echo')
    return dict(cases=rows, summary=summaries[0])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--cases', required=True)
    parser.add_argument('--public-entry', action='store_true')
    parser.add_argument('--negative-tls', action='store_true')
    parser.add_argument('--evidence')
    args = parser.parse_args()
    transports = ['tcp', 'tls'] if args.public_entry else ['full']
    for transport in transports:
        with tempfile.TemporaryDirectory(prefix='h2-mqtt-e2e-') as directory, Fixture(directory, allow_smoke=args.public_entry) as fixture:
            binary = str(Path(args.binary).resolve())
            env = dict(os.environ)
            if args.public_entry:
                for key in list(env):
                    if key.startswith('H2_MQTT_SMOKE_'):
                        del env[key]
                env.update(H2_MQTT_SMOKE_HOST='127.0.0.1', H2_MQTT_SMOKE_PORT=str(fixture.tls.port if transport == 'tls' else fixture.tcp.port),
                           H2_MQTT_SMOKE_TLS='1' if transport == 'tls' else '0', H2_MQTT_SMOKE_CA_FILE=str(fixture.ca),
                           H2_MQTT_SMOKE_TOPIC_PREFIX='h2/mqtt/' + fixture.session)
                command = [binary]
            else:
                command = [binary, str(fixture.tcp.port), str(fixture.tls.port), fixture.session, str(fixture.ca),
                           str(fixture.ca if args.negative_tls else fixture.wrong_ca)]
            completed = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            print(completed.stdout, end='')
            if args.negative_tls:
                rows = [json.loads(line.split(' ', 1)[1]) for line in completed.stdout.splitlines() if line.startswith('H2_PAL_MQTT_CASE ')]
                expected = re.findall(r'H2_PAL_MQTT_CASE\(\w+, "([^"]+)"\)', Path(args.cases).read_text())
                if completed.returncode == 0 or [row['id'] for row in rows] != expected or any(
                        row['status'] != ('FAIL' if row['id'] == 'tls-untrusted' else 'PASS') for row in rows):
                    raise RuntimeError('trusted endpoint was incorrectly accepted as TLS rejection')
                summaries = [json.loads(line.split(' ', 1)[1]) for line in completed.stdout.splitlines()
                             if line.startswith('H2_PAL_MQTT_SUMMARY ')]
                if summaries != [dict(selected=len(expected), passed=len(expected) - 1, failed=1, blocked=0,
                                     retained_allocations=0, invalid_frees=0, cleanup=0)]:
                    raise RuntimeError('negative TLS run failed beyond the expected rejection assertion')
                try:
                    fixture.verify()
                except RuntimeError as error:
                    if 'TLS rejection lacks' not in str(error):
                        raise
                    print('Rejected reused/trusted TLS rejection evidence')
                else:
                    raise RuntimeError('trusted endpoint acquired TLS rejection witness')
                continue
            report = validate(completed.stdout, args.cases, args.public_entry)
            if completed.returncode != 0:
                raise RuntimeError('MQTT host runner failed')
            report['fixture'] = fixture.verify(full=not args.public_entry)
            report['artifact_sha256'] = hashlib.sha256(Path(binary).read_bytes()).hexdigest()
            report['registry_sha256'] = hashlib.sha256(Path(args.cases).read_bytes()).hexdigest()
            if args.evidence:
                Path(args.evidence).write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
