"""Exercise generated component registration without a native SDK installation."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    generated = Path(sys.argv[1]).resolve()
    gizos = Path(sys.argv[2]).resolve()
    fixtures = (
        ("esp", "private_esp_task_policy", "h2_esp", "idf", "h2_firmware_lib;h2_pal_core"),
        ("ap", "private_bk_task_policy/ap", "h2_bk", "armino", "h2_firmware_lib;h2_pal_core;bk_rtos"),
        ("cp", "private_bk_task_policy/cp", "h2_bk", "armino", "h2_pal_core;bk_rtos"),
    )
    with tempfile.TemporaryDirectory(prefix="task-policy-cmake-") as temporary:
        root = Path(temporary)
        # Include both BK execution units in one manifest to detect wrong lookup.
        manifest = root / "manifest.cmake"
        lines = ["set(H2_BAZEL_COMPONENT_NAMES fixture)"]
        for unit, directory, prefix, _, _ in fixtures:
            component = prefix + "_target_task_policy"
            key = ((unit + "_") if unit != "esp" else "") + component
            source = generated / directory / (component + ".c")
            assert source.is_file(), source
            lines.append(f'set(H2_BAZEL_COMPONENT_SRCS_{key.upper()} "{source}")')
        manifest.write_text("\n".join(lines) + "\n")
        for unit, directory, prefix, register, requires in fixtures:
            component = prefix + "_target_task_policy"
            source = generated / directory / (component + ".c")
            header = generated / directory / (component + ".h")
            cmake = generated / directory / "CMakeLists.txt"
            assert header.is_file() and cmake.is_file()
            probe = root / (unit + ".cmake")
            probe.write_text(f'''
function({register}_component_register)
  cmake_parse_arguments(P "" "" "SRCS;INCLUDE_DIRS;REQUIRES" ${{ARGN}})
  if(NOT P_SRCS STREQUAL "{source}")
    message(FATAL_ERROR "wrong execution unit/source: ${{P_SRCS}}")
  endif()
  if(NOT P_INCLUDE_DIRS STREQUAL "." OR NOT P_REQUIRES STREQUAL "{requires}")
    message(FATAL_ERROR "wrong component dependencies or header include directory")
  endif()
endfunction()
include("{cmake}")
''')
            environment = dict(os.environ, H2_GIZOS_ROOT=str(gizos),
                               H2_BAZEL_COMPONENT_MANIFEST=str(manifest))
            subprocess.run(["cmake", "-P", str(probe)], env=environment, check=True)
    print("ESP, BK AP/CP generated CMake registration and manifest lookup passed")


if __name__ == "__main__":
    main()
