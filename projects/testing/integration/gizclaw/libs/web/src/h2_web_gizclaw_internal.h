#ifndef H2_WEB_GIZCLAW_INTERNAL_H
#define H2_WEB_GIZCLAW_INTERNAL_H
#include "h2_web_main_thread.h"
void h2_web_gizclaw_bind_js(void*, h2_web_main_result_t*,
                            h2_web_main_completion_t*);
void h2_web_gizclaw_finish_js(void*, h2_web_main_result_t*,
                              h2_web_main_completion_t*);
void h2_web_gizclaw_detach_js(void*, h2_web_main_result_t*,
                              h2_web_main_completion_t*);
#endif
