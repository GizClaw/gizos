# AC707N Chip Board

AC707N/BR35 compile fixture for the existing portable PAL E2E App. The
repository-owned layout starts UCOS and the PAL task on CPU0, using the pinned
SDK startup, clock, memory, scheduler and linker inputs.

On Linux x86_64 (macOS uses a Linux/amd64 Docker container):

```sh
bazel build --config=ac707n //projects/e2e/targets/native_firmware/pal/ac707n_chip:firmware
bazel build --config=ac707n //projects/e2e/targets/h2loader_tar_zlib/pal/ac707n_chip:package
```

Both entries share `projects/e2e/native_component_src/jieli/br35/pal`; the
portable App stays in `projects/e2e/apps/pal/app`.

Set `JIELI_TOOLCHAIN_ROOT`, `JIELI_POSTBUILD_ROOT`, and `JIELI_AC707N_SDK_PATH`
from firmware-devenv. The firmware exports ELF, NOR, FW/UFW and a dependency
manifest. The package contains the UFW at `app/jieli/update.ufw`.

The fixture assumes 8 MiB Flash, 24 MHz crystal and PB07 reset; no UART is
selected. Inspect `h2_ac707n_pal_e2e_result` and `h2_ac707n_pal_e2e_status`
for test results. No physical board has been tested. The H2Loader device-side
UFW installer and Loader/App boot selection are not implemented by this fixture.
