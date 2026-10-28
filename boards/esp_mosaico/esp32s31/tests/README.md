# Mosaico driver regression ownership

Physical devices implement the existing PAL contracts through the Mosaico
board adapters. Device IDs, GPIO/I2C wiring, unit conversion and cleanup stay
in `boards/esp_mosaico/esp32s31`; they do not define a portable `peripherals`
App or a new PAL interface.

The board regression targets are:

| Target | Owner and checks |
| --- | --- |
| `button_driver_test` | Board device registry and AI/BOOT level mapping |
| `imu_driver_test`, `mag_test` | BMI270/BMM150 conversion, invalid readings and output cleanup |
| `battery_driver_test` | BQ27220 read-only commands, signed current, percent range and I/O failures |
| `lifecycle_driver_test` | Init retry, resource release retry, retained handles and idempotent shutdown |
| `surface_test` | Display/touch coordinate surface boundaries |

Lifecycle regression uses fake PWM callbacks to verify resource shutdown; it
does not drive a physical motor. There is no standalone motor diagnostic
sequence, timed vibration pulse or interactive `peripherals` firmware mode.
Touch hardware is adapted by `src/h2_mosaico_display.c` using the existing
PAL touch API; sensor and battery adapters remain in the board driver owner.

E2E Apps remain under `projects/e2e/apps`. Their inventory includes GizClaw,
atomic, Lua and performance Apps, not only names beginning with `pal-`.
Use `//projects/e2e/tools:list_apps` to enumerate them.

H2Loader is a product firmware entry under `projects/h2loader/targets`.
The S31 `reference-smoke` entry is a toolchain compile/link check using a
compile-only layout. Neither is an E2E App or a selectable Mosaico E2E suite.
