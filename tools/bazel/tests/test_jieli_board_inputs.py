"""The public board aggregate admits only owned inputs, never stray files."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BoardInputsTest(unittest.TestCase):
    def test_unowned_files_are_excluded(self):
        groups = []
        # Model files appearing at the package root without creating repository
        # debris. Bazel stops recursive globbing at the ac791n subpackage.
        namespace = {
            'package': lambda **kwargs: None,
            'filegroup': lambda **kwargs: groups.append(kwargs),
            'select': lambda choices: [],
            'glob': lambda *args, **kwargs: ['scratch.txt', '.DS_Store'],
        }
        source = ROOT / 'boards/jieli_ac791n_devkit/BUILD.bazel'
        exec(compile(source.read_text(), str(source), 'exec'), namespace)
        self.assertEqual(groups[0]['srcs'], ['//boards/jieli_ac791n_devkit/ac791n'])


if __name__ == '__main__':
    unittest.main()
