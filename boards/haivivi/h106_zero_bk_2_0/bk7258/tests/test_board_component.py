"""Execute the staged BSP CMake entry with an independent consumer root."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BoardComponentTest(unittest.TestCase):
    def test_staged_external_consumer(self):
        board_relative = Path("boards/haivivi/h106_zero_bk_2_0/bk7258")
        # Keep runfile paths: resolving a source symlink loses the runfiles root.
        gizos = Path(__file__).absolute().parents[5]
        board = gizos / board_relative
        with tempfile.TemporaryDirectory(prefix="zero-bk-component-") as temporary:
            root = Path(temporary)
            consumer = root / "consumer"
            consumer.mkdir()
            staged = root / "components/ap/h2_bk7258_board"
            staged.mkdir(parents=True)
            for name in ("CMakeLists.txt", "board_component.cmake"):
                shutil.copyfile(board / "ap" / name, staged / name)
            source = board / "ap/src/h2_bk7258_board_led.c"
            manifest = root / "manifest.cmake"
            manifest.write_text(
                "set(H2_BAZEL_COMPONENT_NAMES h2_bk7258_board)\n"
                f'set(H2_BAZEL_COMPONENT_SRCS_AP_H2_BK7258_BOARD "{source}")\n'
            )
            probe = root / "probe.cmake"
            probe.write_text(f'''
cmake_policy(SET CMP0057 NEW)
function(armino_component_register_ex)
  cmake_parse_arguments(P "" "CONFIG_COMPONENT_NAME" "SRCS;INCLUDE_DIRS;PRIV_REQUIRES" ${{ARGN}})
  if(NOT P_CONFIG_COMPONENT_NAME STREQUAL "h2_bk7258_board" OR
     NOT P_SRCS STREQUAL "{source}")
    message(FATAL_ERROR "wrong component/source registration")
  endif()
  set(EXPECTED "{board}/include;{board}/include;{board}/ap/include;{board}/ap/src")
  if(NOT P_INCLUDE_DIRS STREQUAL EXPECTED)
    message(FATAL_ERROR "BSP includes escaped GizOS: ${{P_INCLUDE_DIRS}}")
  endif()
  foreach(DIRECTORY IN LISTS P_INCLUDE_DIRS)
    if(NOT IS_DIRECTORY "${{DIRECTORY}}")
      message(FATAL_ERROR "missing BSP include directory: ${{DIRECTORY}}")
    endif()
  endforeach()
  if(NOT "h2_pal_core" IN_LIST P_PRIV_REQUIRES)
    message(FATAL_ERROR "missing native provider dependency")
  endif()
  set(REGISTERED TRUE PARENT_SCOPE)
endfunction()
include("{staged}/CMakeLists.txt")
if(NOT REGISTERED)
  message(FATAL_ERROR "BSP was not registered")
endif()
''')
            subprocess.run(
                ["cmake", "-P", str(probe)],
                env=dict(os.environ, H2_REPO_ROOT=str(consumer),
                         H2_GIZOS_ROOT=str(gizos),
                         H2_BAZEL_COMPONENT_MANIFEST=str(manifest)),
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
