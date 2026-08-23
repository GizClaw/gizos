# Display Smoke App

This app draws a full-screen RGB565 color bar and then holds that visual scene
for human inspection.

The shared Smoke App lives under `app/` and only uses PAL display APIs. Target
entrypoints and product lifecycle integration are owned by consumer launchers;
H2Loader launchers continue to own their image identity and confirmation flow.

Expected inspection:

- The display turns on at visible brightness.
- The screen shows stable vertical color bars.
- The scene does not advance to another screen.
- LVGL is not initialized for this smoke app.
