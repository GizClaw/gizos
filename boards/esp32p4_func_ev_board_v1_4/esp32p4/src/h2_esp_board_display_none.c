#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"

#include "esp_log.h"

static const char *TAG = "h2_esp_display";

h2_pal_display_t *h2_esp_board_display(void) {
    static int warned;
    if (!warned) {
        ESP_LOGW(TAG, "board %s has no display backend", H2_ESP_BOARD_NAME);
        warned = 1;
    }
    return NULL;
}

h2_pal_display_t *h2_esp_board_display_if_initialized(void) {
    return NULL;
}
