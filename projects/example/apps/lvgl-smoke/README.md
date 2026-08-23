# LVGL Smoke App

`lvgl-smoke` is a shared portable Smoke App for validating LVGL platform integration. It uses Runtime Display, Task, Sync, Queue, Time, and Memory capabilities without including a board, H2Loader, or target SDK header.

The visible screen contains a rounded card, circular color indicators, and a continuously updated rounded progress bar. The App uses one RGB565 buffer covering the complete physical display and configures LVGL with `LV_DISPLAY_RENDER_MODE_FULL`; partial or tiled rendering is not accepted.

The Zero BK 1.0 launcher supplies its PSRAM allocator for both the LVGL heap and full-screen render buffer. The App reports ready only after the first complete frame is presented, then continues running LVGL on the App task.

Expected inspection:

- The screen shows `LVGL SMOKE`.
- All six color indicators are circular.
- The card and progress bar have rounded corners.
- The progress bar updates continuously without page-by-page redraw.
- When consumed as an H2Loader image, status reports the confirmed `lvgl-smoke` App image.
