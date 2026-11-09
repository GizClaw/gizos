#ifndef H2_LVGL_TASK_NAMES_H
#define H2_LVGL_TASK_NAMES_H

#define H2_LVGL_SOFTWARE_DRAW_TASK_NAME_VALUE "$lvgl/swdraw"
#define H2_LVGL_G2D_DRAW_TASK_NAME_VALUE "$lvgl/g2ddraw"
#define H2_LVGL_PXP_DRAW_TASK_NAME_VALUE "$lvgl/pxpdraw"
#define H2_LVGL_NEMA_GFX_TASK_NAME_VALUE "$lvgl/nemagfx"

#ifdef __cplusplus
extern "C" {
#endif

extern const char h2_lvgl_software_draw_task_name[sizeof(
    H2_LVGL_SOFTWARE_DRAW_TASK_NAME_VALUE)];
extern const char
    h2_lvgl_g2d_draw_task_name[sizeof(H2_LVGL_G2D_DRAW_TASK_NAME_VALUE)];
extern const char
    h2_lvgl_pxp_draw_task_name[sizeof(H2_LVGL_PXP_DRAW_TASK_NAME_VALUE)];
extern const char
    h2_lvgl_nema_gfx_task_name[sizeof(H2_LVGL_NEMA_GFX_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
