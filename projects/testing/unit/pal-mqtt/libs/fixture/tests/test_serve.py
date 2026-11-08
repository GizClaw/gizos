import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import serve


class ServeCommandTest(unittest.TestCase):
    def test_documented_explicit_evidence_command_starts_real_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            arguments=['serve','--bind','127.0.0.1','--advertised','127.0.0.1',
                       '--bazelrc',str(root/'fixture.bazelrc'),'--receipt',str(root/'receipt.json'),
                       '--evidence',str(root/'inputs')]
            output=io.StringIO()
            with patch.object(sys,'argv',arguments), patch.object(serve.time,'sleep',side_effect=KeyboardInterrupt), \
                 contextlib.redirect_stdout(output):
                serve.main()
            ready=json.loads(output.getvalue())
            self.assertEqual(ready['status'],'ready')
            self.assertGreater(ready['tcp_port'],0)
            self.assertGreater(ready['tls_port'],0)
            receipt=json.loads((root/'receipt.json').read_text())
            inputs=json.loads((root/'inputs'/'inputs.json').read_text())
            self.assertEqual(receipt['inputs'],inputs)
            self.assertEqual(inputs['session_prefix'],ready['session_prefix'])
            self.assertEqual((root/'fixture.bazelrc').read_bytes(),(root/'inputs'/'fixture.bazelrc').read_bytes())
            self.assertTrue((root/'inputs'/'ca.pem').is_file())


if __name__=='__main__':unittest.main()
