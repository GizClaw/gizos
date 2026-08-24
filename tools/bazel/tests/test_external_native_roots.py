from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]


class ExternalNativeRootsTest(unittest.TestCase):
    def test_lvgl_components_use_the_canonical_gizos_root(self) -> None:
        paths = (
            ROOT / "native_component_src/esp-idf6.x/lvgl_port/CMakeLists.txt",
            ROOT / "native_component_src/bk7258/ap/lvgl_port/CMakeLists.txt",
        )
        for path in paths:
            with self.subTest(path=path.relative_to(ROOT)):
                text = path.read_text(encoding="utf-8")
                self.assertIn('set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")', text)
                self.assertNotIn("H2_REPO_ROOT", text)

    def test_native_sdk_watchers_exclude_git_internals(self) -> None:
        paths = (
            ROOT / "tools/bazel/native_repositories/esp_repositories.bzl",
            ROOT / "tools/bazel/native_repositories/bk_repositories.bzl",
            ROOT / "tools/bazel/native_repositories/repositories.bzl",
        )
        for path in paths:
            with self.subTest(path=path.relative_to(ROOT)):
                text = path.read_text(encoding="utf-8")
                self.assertIn('if candidate.basename == ".git":', text)
                self.assertIn("repository_ctx.watch_tree(candidate)", text)
                self.assertIn("repository_ctx.watch(candidate)", text)
                self.assertNotIn("exclude =", text)

    def test_esp_gizclaw_declares_its_opus_component_dependency(self) -> None:
        path = ROOT / "native_component_src/esp-idf6.x/h2_gizclaw/CMakeLists.txt"
        text = path.read_text(encoding="utf-8")
        requires = text.partition("REQUIRES")[2].partition(")")[0].split()
        self.assertIn("opus_port", requires)


if __name__ == "__main__":
    unittest.main()
