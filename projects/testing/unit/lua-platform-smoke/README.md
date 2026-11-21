# Runtime/Lua platform smoke

This single-DUT workload creates a Runtime-backed Lua Host, executes a sleeping job and checks its returned value through the public APIs. Its C3 entry is `//projects/testing/unit/targets/esp_idf_firmware/lua-platform-smoke/esp32c3:firmware`. The entry owns an OS-only Runtime configuration, internal-stack policy and a factory-only test partition map; it is not a physical board BSP or an H2Loader upgrade image. The historical firmware identity `esp32c3_chip` is retained for this diagnostic image.

A successful build checks the declared C3/native ABI. Execution, hardware peripherals, product memory budgets and Loader recovery each require their own evidence; building this target does not qualify them. The AI Passport Loader and MFG entries use the real FoloToy board and its independent product layout.
