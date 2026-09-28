"""Run the production host HTTP provider against owned network peers."""
import argparse
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
from fixture import Fixture


def validate(text, cases):
    rows = [json.loads(line.split(' ', 1)[1]) for line in text.splitlines() if line.startswith('H2_PAL_HTTP_CASE ')]
    expected = re.findall(r'H2_PAL_HTTP_CASE\(\w+, "([^"]+)"\)', Path(cases).read_text())
    if [row['id'] for row in rows] != expected or any(row['status'] != 'PASS' for row in rows):
        raise RuntimeError('mandatory HTTP case registry did not pass')
    summaries = [json.loads(line.split(' ', 1)[1]) for line in text.splitlines() if line.startswith('H2_PAL_HTTP_SUMMARY ')]
    if summaries != [dict(passed=len(expected), failed=0, blocked=0, retained_allocations=0, cleanup=0)]:
        raise RuntimeError('HTTP summary or cleanup failed')
    return dict(schema=1, platform='macos' if __import__('sys').platform == 'darwin' else 'linux',
                case_count=len(expected), passed=len(expected), failed=0, blocked=0,
                tls_verification='required; isolated test CA; separate untrusted certificate rejected',
                cases=rows, summary=summaries[0])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--cases', required=True)
    parser.add_argument('--evidence', default=str(Path(os.environ['TEST_UNDECLARED_OUTPUTS_DIR']) / 'qualified.json') if 'TEST_UNDECLARED_OUTPUTS_DIR' in os.environ else None)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='h2-http-e2e-') as temp:
        with Fixture(temp) as fixture:
            completed = subprocess.run([str(Path(args.binary).resolve()), fixture.http, fixture.https,
                                        fixture.untrusted, str(fixture.ca)], text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
            print(completed.stdout, end='')
            report = validate(completed.stdout, args.cases)
            if completed.returncode:
                raise RuntimeError('host runner failed')
            report['artifact_sha256'] = hashlib.sha256(Path(args.binary).read_bytes()).hexdigest()
            report['registry_sha256'] = hashlib.sha256(Path(args.cases).read_bytes()).hexdigest()
            report['fixture_attempts'] = fixture.verify_arrivals()
            if args.evidence:
                Path(args.evidence).write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
