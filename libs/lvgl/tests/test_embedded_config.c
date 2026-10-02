#include "lvgl.h"

/* Deliberately consume generic headers alongside an embedded config. The
 * selected configuration must survive their conflicting lv_conf.h name. */
_Static_assert(LV_COLOR_DEPTH == 16, "embedded native pixel depth");
_Static_assert(LV_DEF_REFR_PERIOD == 33, "embedded refresh period");
_Static_assert(LV_USE_OS == LV_OS_CUSTOM, "embedded PAL task backend");
_Static_assert(LV_USE_OBJ_NAME == 0, "embedded object layout matches SDK config");
_Static_assert(LV_DRAW_SW_DRAW_UNIT_CNT == 1, "embedded software draw units");

int main(void) {
    return 0;
}
