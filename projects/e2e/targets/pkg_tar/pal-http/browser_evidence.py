"""Fail-closed interpretation of C Worker and browser TLS observations."""
import json


class WorkerEvidenceError(RuntimeError):
    pass


def validate_worker_state(lines):
    records = [json.loads(line.split(' ', 1)[1]) for line in lines
               if line.startswith('H2_PAL_HTTP_WORKER_STATE ')]
    if len(records) != 1:
        raise WorkerEvidenceError('exactly one C pthread state record is required')
    state = records[0]
    expected = dict(schema=1, caller_is_pthread=1, caller_is_main_runtime=0,
                    registry_present=1, registry_valid=1, pending_requests=0)
    for key, value in expected.items():
        if type(state.get(key)) is not int or state[key] != value:
            raise WorkerEvidenceError('invalid or missing Worker observation: ' + key)
    if type(state.get('registry_owner_is_pthread')) is not int or state['registry_owner_is_pthread'] not in (0, 1):
        raise WorkerEvidenceError('actual registry owner context is missing')
    return state


def validate_certificate_error(failures, expected_url):
    matches = [event for event in failures
               if event.get('url') == expected_url and
               event.get('error') == 'net::ERR_CERT_AUTHORITY_INVALID']
    if not matches:
        raise RuntimeError('exact untrusted endpoint did not produce a browser certificate authority rejection')
    return matches
