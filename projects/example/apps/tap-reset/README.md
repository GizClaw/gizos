# Tap Reset Smoke App

This project owns the target-independent Tap Reset LVGL App. Its blocking
public entry accepts `h2_runtime_t *` plus stable App-level presentation,
pointer, and lifecycle config.

Memory, Display, and Time are required Runtime capabilities. Pointer remains a
stable App config callback until Firmwares defines a public pointer/touch PAL;
it must not contain UIKit, JNI, Emscripten, board, or launcher-private types.

Mobile packaging and simulator/browser commands live in
`guides/apps/mobile.md`. Desktop or device artifact entries can consume this App
directly without depending on the mobile adapter.
