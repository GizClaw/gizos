#ifndef H2_BK_TEST_CONSOLE_LOG_H
#define H2_BK_TEST_CONSOLE_LOG_H

#include <stdio.h>

/* The pinned SDK's async printf reports formatting success even when its log
 * pool drops the record. Exercise that failure beneath the real Log provider.
 */
int h2_test_bk_async_printf(const char* format, ...);
#define printf h2_test_bk_async_printf
#define BK_LOGD(tag, ...) ((void)(tag), h2_test_bk_async_printf(__VA_ARGS__))
#define BK_LOGI(tag, ...) ((void)(tag), h2_test_bk_async_printf(__VA_ARGS__))
#define BK_LOGW(tag, ...) ((void)(tag), h2_test_bk_async_printf(__VA_ARGS__))
#define BK_LOGE(tag, ...) ((void)(tag), h2_test_bk_async_printf(__VA_ARGS__))

#endif
