#ifndef TEST_ARENA_ESP_LOG_H
#define TEST_ARENA_ESP_LOG_H
void test_arena_log(const char *tag, const char *format, ...);
#define ESP_LOGW(tag, ...) test_arena_log(tag, __VA_ARGS__)
#endif
