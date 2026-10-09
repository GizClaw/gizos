# AC707N Chip Board

AC707N/BR35 board and compile-only layout using the pinned SDK startup, clock,
memory, scheduler and linker inputs. The mixed PAL E2E launcher, native firmware
and H2Loader package targets have been retired. The independent PAL Core App
does not yet have an AC707N launcher. JieLi had no PAL E2E v2 acceptance run
when the old entries were retired and is now included in the follow-up platform
coverage scope. [The E2E Apps README](../../projects/e2e/apps/README.md) records
Linux, Windows and JieLi as the three remaining platform gaps.

Set `JIELI_TOOLCHAIN_ROOT`, `JIELI_POSTBUILD_ROOT`, and `JIELI_AC707N_SDK_PATH`
from firmware-devenv. Its `make jieli-ac707n-toolchain-check` can check BR35
compiler flags and the `r3-large` runtime link on Linux x86_64; it does not
replace an E2E firmware build or a physical qualification run.

The fixture assumes 8 MiB Flash, 24 MHz crystal and PB07 reset; no UART is
selected. The retired fixture had no physical-board qualification. The
H2Loader device-side UFW installer and Loader/App boot selection still require
their own implementation and validation.
