import json
import unittest
from browser_evidence import WorkerEvidenceError, validate_worker_state, validate_certificate_error


class BrowserEvidence(unittest.TestCase):
    def test_missing_wrong_context_and_non_numeric_states_fail_closed(self):
        valid = dict(schema=1, caller_is_pthread=1, caller_is_main_runtime=0,
                     registry_present=1, registry_valid=1, pending_requests=0,
                     registry_owner_is_pthread=0)
        validate_worker_state(['H2_PAL_HTTP_WORKER_STATE ' + json.dumps(valid)])
        invalid = [[], ['H2_PAL_HTTP_SUMMARY {}']]
        for key in valid:
            missing = valid.copy()
            del missing[key]
            invalid.append(['H2_PAL_HTTP_WORKER_STATE ' + json.dumps(missing)])
        for pending in [None, False, '0', 0.0, -1, 1]:
            invalid.append(['H2_PAL_HTTP_WORKER_STATE ' + json.dumps(dict(valid, pending_requests=pending))])
        invalid.append(['H2_PAL_HTTP_WORKER_STATE ' + json.dumps(dict(valid, caller_is_pthread=0))])
        for lines in invalid:
            with self.assertRaises(WorkerEvidenceError):
                validate_worker_state(lines)

    def test_generic_network_error_or_other_url_cannot_prove_tls_rejection(self):
        url = 'https://fixture.invalid/negative/bytes'
        for failures in [[], [dict(url=url, error='net::ERR_CONNECTION_REFUSED')],
                         [dict(url=url + '/other', error='net::ERR_CERT_AUTHORITY_INVALID')]]:
            with self.assertRaises(RuntimeError):
                validate_certificate_error(failures, url)
        self.assertEqual(len(validate_certificate_error([dict(url=url, error='net::ERR_CERT_AUTHORITY_INVALID')], url)), 1)


if __name__ == '__main__':
    unittest.main()
