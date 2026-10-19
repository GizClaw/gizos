#include "lvgl.h"

/* Deliberately consume generic headers alongside an embedded config. The
 * selected configuration must survive their conflicting lv_conf.h name. */
/* Negative array bounds also reject mismatches in MSVC's default C mode. */
typedef char h2_lvgl_config_pixel_depth[(LV_COLOR_DEPTH == 16) ? 1 : -1];
typedef char h2_lvgl_config_refresh_period[(LV_DEF_REFR_PERIOD == 33) ? 1 : -1];
typedef char h2_lvgl_config_task_backend[(LV_USE_OS == LV_OS_CUSTOM) ? 1 : -1];
typedef char h2_lvgl_config_object_layout[(LV_USE_OBJ_NAME == 0) ? 1 : -1];
typedef char h2_lvgl_config_draw_units[(LV_DRAW_SW_DRAW_UNIT_CNT == 1) ? 1 : -1];

int main(void) {
    return 0;
}
