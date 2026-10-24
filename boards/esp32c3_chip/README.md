# ESP32-C3 Chip Board

Bare ESP32-C3 profile with 8 MiB Flash, internal RAM only and USB Serial/JTAG console. The BSP supplies an OS-only Runtime configuration; physical peripherals and network services are explicitly unsupported in this fixture.

The `compile_only` layout has one factory partition. It validates native compilation and the standalone Lua platform smoke; it is not an H2Loader upgrade layout. AI Passport wiring, product Runtime peripherals and H2Loader partition policy belong to the consuming product board.
