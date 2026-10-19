"""Exercise the actual imported-archive CMake graph and GNU cyclic link."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HELPER = Path(__file__).resolve().parents[1] / "h2_bazel_archive.cmake"

class ArchiveGroupTest(unittest.TestCase):
    def test_imported_archives_stay_inside_group(self):
        cmake = shutil.which("cmake")
        if not cmake or not shutil.which("ninja") or not (shutil.which("cc") or shutil.which("cl")):
            self.skipTest("CMake/Ninja unavailable")
        with tempfile.TemporaryDirectory(prefix="h2-rescan-") as temporary:
            root = Path(temporary)
            for file, text in {
                "a.c": "int b(void); int a(void) { return b(); }\n",
                "later.c": "int later(void) { return 7; }\n",
                "b.c": "int later(void); int b(void) { return later(); }\n",
                "main.c": "int a(void); int main(void) { return a() == 7 ? 0 : 1; }\n",
            }.items():
                (root / file).write_text(text)
            (root / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.24)
project(rescan C)
add_library(source_a STATIC a.c later.c)
add_library(source_b STATIC b.c)
add_library(firmware INTERFACE)
function(add_prebuilt_library name path)
  add_library(${{name}} STATIC IMPORTED GLOBAL)
  set_target_properties(${{name}} PROPERTIES IMPORTED_LOCATION "${{path}}")
endfunction()
set(H2_ARCHIVE "${{CMAKE_BINARY_DIR}}/libsource_a.a")
set(H2_ARCHIVE_DEPENDENCY_0 "${{CMAKE_BINARY_DIR}}/libsource_b.a")
file(TOUCH "${{H2_ARCHIVE}}" "${{H2_ARCHIVE_DEPENDENCY_0}}")
set(COMPONENT_LIB firmware)
include("{HELPER.as_posix()}")
h2_idf_import_bazel_archive(firmware H2_ARCHIVE)
add_executable(probe main.c)
add_dependencies(probe source_a source_b)
target_link_libraries(probe PRIVATE "-Wl,--start-group" firmware "-Wl,--end-group")
''')
            configured = subprocess.run([cmake, "-G", "Ninja", "-S", str(root), "-B", str(root / "build")], capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            ninja = (root / "build/build.ninja").read_text()
            flags = re.search(r"LINK_LIBRARIES = (.*)", ninja)
            self.assertIsNotNone(flags)
            items = flags[1].split()
            groups = []
            start = None
            for index, item in enumerate(items):
                if item == "-Wl,--start-group":
                    start = index
                elif item == "-Wl,--end-group" and start is not None:
                    groups.append(items[start + 1:index])
                    start = None
            self.assertTrue(any(
                any("libsource_a.a" in item for item in group) and
                any("libsource_b.a" in item for item in group)
                for group in groups
            ), flags[1])
            # Apple's linker lacks GNU archive groups. Generation is still
            # checked there; Linux exercises the real a -> b -> later cycle.
            cc = shutil.which("cc")
            compiler = subprocess.run([cc, "-Wl,--version"], capture_output=True, text=True) if cc else None
            if compiler and "GNU" in compiler.stdout + compiler.stderr:
                built = subprocess.run([cmake, "--build", str(root / "build")], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                subprocess.run([str(root / "build/probe")], check=True)

if __name__ == "__main__":
    unittest.main()
