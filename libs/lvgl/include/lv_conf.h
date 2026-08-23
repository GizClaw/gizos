#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32
#define LV_USE_OS LV_OS_CUSTOM
#define LV_OS_CUSTOM_INCLUDE "h2_lvgl_osal.h"

#define LV_DEF_REFR_PERIOD 16
#define LV_DPI_DEF 130

#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_USE_DRAW_SW 1
#ifndef LV_USE_DRAW_SDL
#define LV_USE_DRAW_SDL 0
#endif
#ifndef LV_USE_SDL
#define LV_USE_SDL 0
#endif

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_INFO

#define LV_USE_OBSERVER 1
#define LV_USE_TRANSLATION 1
#define LV_USE_OBJ_NAME 1
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 1
#define LV_FONT_MONTSERRAT_42 1
#define LV_TINY_TTF_CACHE_GLYPH_CNT 64
/* LVGL 9.3 places the cache header immediately after a 12-byte kerning key,
 * which is not 8-byte aligned on 64-bit Desktop targets. Keep glyph caching
 * enabled, but calculate kerning pairs directly until the cache layout is
 * alignment-safe. */
#define LV_TINY_TTF_CACHE_KERNING_CNT 0
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

#endif
