"""Check outputs produced by the real JieLi Starlark macro and graph aspect."""

from pathlib import Path
import sys
import unittest


class GeneratedPolicyTest(unittest.TestCase):
    def test_generated_source(self):
        self.assertEqual(Path(sys.argv[1]).read_text(), """/* Generated from a target-owned Bazel policy. Do not edit. */
#include "system/task.h"
const struct task_info task_info_table[] = {
    {"sdk_worker", 5u, 512u, 8u, 0},
    {"#C1fixture/worker", 7u, 1024u, 16u, 0},
    {0, 0, 0, 0, 0},
};
const struct task_info h2_jieli_default_task_policy = {"h2_default", 3u, 256u, 0u, 0};
""")

    def test_audit_task_list(self):
        self.assertEqual(Path(sys.argv[2]).read_text(), "fixture/worker\n")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
