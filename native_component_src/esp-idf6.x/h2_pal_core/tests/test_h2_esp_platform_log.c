#include "h2_esp_platform_core.h"
#include "esp_log.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static esp_log_level_t core_level = ESP_LOG_WARN;
static esp_log_level_t master_level = ESP_LOG_WARN;
static unsigned setter_calls, output_count;
static unsigned master_setter_calls, master_getter_calls;
static char output[512];

void esp_log_level_set(const char *tag, esp_log_level_t level) {
    assert(strcmp(tag, "pal-core") == 0);
    core_level = level;
    ++setter_calls;
}

esp_log_level_t esp_log_level_get(const char *tag) {
    return strcmp(tag, "pal-core") == 0 ? core_level : ESP_LOG_WARN;
}

void esp_log_set_level_master(esp_log_level_t level) {
    master_level = level;
    ++master_setter_calls;
}

esp_log_level_t esp_log_get_level_master(void) {
    ++master_getter_calls;
    return master_level;
}

void h2_test_log_emit(esp_log_level_t level, const char *tag,
                      const char *format, ...) {
    assert(level >= ESP_LOG_ERROR && level <= ESP_LOG_DEBUG);
    if (level > esp_log_level_get(tag)) return;
    static const char names[] = "?EWID";
    int prefix = snprintf(output, sizeof(output), "%c (123) %s: ", names[level], tag);
    assert(prefix > 0 && (size_t)prefix < sizeof(output));
    va_list args;
    va_start(args, format);
    int body = vsnprintf(output + prefix, sizeof(output) - (size_t)prefix, format, args);
    va_end(args);
    assert(body >= 0 && (size_t)body + (size_t)prefix + 2u <= sizeof(output));
    memcpy(output + prefix + body, "\n", 2u);
    ++output_count;
}

static void write_levels(const h2_pal_log_api_t *log, int debug_enabled,
                         esp_log_level_t master) {
    static const char names[] = "DIWE";
    static const esp_log_level_t native[] = {
        ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_WARN, ESP_LOG_ERROR};
    unsigned setters_before = setter_calls;
    unsigned master_setters_before = master_setter_calls;
    for (int level = H2_PAL_LOG_DEBUG; level <= H2_PAL_LOG_ERROR; ++level) {
        unsigned before = output_count;
        assert(h2_pal_log_write(log, (h2_pal_log_level_t)level, "pal-core", "body") == H2_PAL_OK);
        int visible = (debug_enabled || level >= H2_PAL_LOG_WARN) &&
                      native[level] <= master;
        assert(output_count == before + (unsigned)visible);
        if (visible) {
            char expected[64];
            snprintf(expected, sizeof(expected), "%c (123) pal-core: body\n", names[level]);
            assert(strcmp(output, expected) == 0);
        }
    }
    assert(setter_calls == setters_before);
    assert(master_setter_calls == master_setters_before);
}

int main(void) {
    const h2_pal_log_api_t *log = h2_esp_platform_log_api();
    write_levels(log, 0, ESP_LOG_WARN);
#if CONFIG_LOG_MASTER_LEVEL
    esp_log_set_level_master(ESP_LOG_DEBUG);
#endif
    esp_log_level_set("pal-core", ESP_LOG_DEBUG);
    write_levels(log, 1, ESP_LOG_DEBUG);
    unsigned before = output_count;
    assert(h2_pal_log_write(log, H2_PAL_LOG_DEBUG, "other", "hidden") == H2_PAL_OK);
    assert(output_count == before && esp_log_level_get("other") == ESP_LOG_WARN);
    esp_log_level_set("pal-core", ESP_LOG_WARN);
    write_levels(log, 0, ESP_LOG_DEBUG);

    char maximum[H2_PAL_LOG_MESSAGE_MAX];
    memset(maximum, 'x', sizeof(maximum) - 1u);
    maximum[sizeof(maximum) - 1u] = '\0';
    assert(h2_pal_log_write(log, H2_PAL_LOG_WARN, "pal-core", maximum) == H2_PAL_OK);
    char expected[512];
    snprintf(expected, sizeof(expected), "W (123) pal-core: %s\n", maximum);
    assert(strcmp(output, expected) == 0);
    assert(h2_pal_log_write(log, H2_PAL_LOG_ERROR, "pal-core", "%n %s %%") == H2_PAL_OK);
    assert(strcmp(output, "E (123) pal-core: %n %s %%\n") == 0);
    assert(h2_pal_log_write(log, H2_PAL_LOG_WARN, NULL, "fallback") == H2_PAL_OK);
    assert(strcmp(output, "W (123) h2: fallback\n") == 0);
    assert(h2_pal_log_write(log, H2_PAL_LOG_WARN, "", "fallback") == H2_PAL_OK);
    assert(strcmp(output, "W (123) h2: fallback\n") == 0);

    before = output_count;
    assert(h2_pal_log_write(log, H2_PAL_LOG_INFO, "pal-core", NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(log->vtable->write(log->user, H2_PAL_LOG_INFO, "pal-core", NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_log_write(log, (h2_pal_log_level_t)-1, "pal-core", "invalid") == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_log_write(log, (h2_pal_log_level_t)4, "pal-core", "invalid") == H2_PAL_ERR_INVALID_ARG);
    assert(output_count == before && setter_calls == 2u);
    assert(esp_log_level_get("pal-core") == ESP_LOG_WARN);
#if CONFIG_LOG_MASTER_LEVEL
    esp_log_level_set("pal-core", ESP_LOG_DEBUG);
    esp_log_set_level_master(ESP_LOG_NONE);
    write_levels(log, 1, ESP_LOG_NONE);
    unsigned getters_before = master_getter_calls;
    assert(log->vtable->write(log->user, H2_PAL_LOG_INFO, NULL, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_log_write(log, (h2_pal_log_level_t)-1, "pal-core", "invalid") == H2_PAL_ERR_INVALID_ARG);
    assert(h2_pal_log_write(log, (h2_pal_log_level_t)4, "pal-core", "invalid") == H2_PAL_ERR_INVALID_ARG);
    assert(master_getter_calls == getters_before);
    esp_log_set_level_master(ESP_LOG_ERROR);
    write_levels(log, 1, ESP_LOG_ERROR);
    esp_log_set_level_master(ESP_LOG_WARN);
    esp_log_level_set("pal-core", ESP_LOG_WARN);
    write_levels(log, 0, ESP_LOG_WARN);
    before = output_count;
    assert(h2_pal_log_write(log, H2_PAL_LOG_DEBUG, "other", "hidden") == H2_PAL_OK);
    assert(output_count == before && esp_log_level_get("other") == ESP_LOG_WARN);
    assert(setter_calls == 4u && master_setter_calls == 4u);
#else
    assert(master_setter_calls == 0u && master_getter_calls == 0u);
#endif
    return 0;
}
