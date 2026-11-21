# Lua integration

The [public Host contract](include/h2_lua.h) defines display buffering, allocator ownership and checked shutdown. A no-PSRAM product using a 240 × 320 RGB565 framebuffer needs one contiguous 153,600-byte pixel allocation outside the VM quota. Reserve it through the Host allocator early enough that Runtime, workers and radio services cannot fragment the required block; use the public framebuffer reservation and single-buffer options where appropriate.

Measure both the largest available block and the complete concurrent workload's memory peak. VM quotas and chip-smoke stack or queue capacities do not establish a product memory budget. Keep the Host and borrowed providers alive until checked worker and display cleanup succeeds.
