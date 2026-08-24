import os
import pathlib
import re
import unittest


class WasmUrlTest(unittest.TestCase):
    def test_every_static_wasm_url_exists_in_the_package(self):
        package = (
            pathlib.Path(os.environ["TEST_SRCDIR"])
            / os.environ["TEST_WORKSPACE"]
            / "projects/h2loader/targets/npm_package/h2loader/h2loader"
        )
        self.assertTrue(package.is_dir(), package)

        references = set()
        for script in package.glob("*.js"):
            references.update(
                re.findall(r"new URL\(['\"]([^'\"]+\.wasm)['\"], import\.meta\.url\)", script.read_text())
            )

        self.assertTrue(references, "package contains no static WASM URL")
        for reference in references:
            self.assertTrue((package / reference).is_file(), reference)


if __name__ == "__main__":
    unittest.main()
