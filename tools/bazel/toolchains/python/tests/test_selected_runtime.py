"""Check py_test and the interpreter target supplied to pip use the same Python."""

import json
import os
from pathlib import Path
import sys
import unittest


class SelectedRuntimeTest(unittest.TestCase):
    def test_toolchain_matches_the_shared_pip_interpreter_target(self):
        expected = Path(os.environ["GIZOS_SELECTED_PYTHON"]).resolve()
        self.assertEqual(Path(sys._base_executable).resolve(), expected)
        metadata = json.loads(Path(os.environ["GIZOS_PYTHON_RUNTIME"]).read_text(encoding="utf-8"))
        self.assertEqual(f"{sys.version_info.major}.{sys.version_info.minor}", metadata["requested"])
        if metadata["enabled"]:
            self.assertEqual(".".join(str(part) for part in sys.version_info[:3]), metadata["version"])
            self.assertEqual(str(expected), metadata["interpreter"])


if __name__ == "__main__":
    unittest.main()
