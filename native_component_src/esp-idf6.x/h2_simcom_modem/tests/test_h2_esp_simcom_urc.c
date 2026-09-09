#include "h2_esp_simcom_urc.h"
#include "h2_simcom_modem.h"
#include <assert.h>
#include <string.h>

typedef struct receiver {
    unsigned calls;
    h2_pal_result_t result;
    char line[H2_SIMCOM_LINE_MAX];
} receiver_t;

static h2_pal_result_t post(void *user, const char *line) {
    receiver_t *rx = user;
    rx->calls++;
    strcpy(rx->line, line);
    return rx->result;
}

int main(void) {
    receiver_t rx = {0};
    const uint8_t input[] = "\r\nRING\r\nNO CARRIER\r\n";
    assert(h2_esp_simcom_forward_urcs(&rx, input, sizeof(input) - 1u, post) == ESP_OK);
    assert(rx.calls == 2u && strcmp(rx.line, "NO CARRIER") == 0);
    rx = (receiver_t){.result = H2_PAL_ERR_FULL};
    assert(h2_esp_simcom_forward_urcs(&rx, input, sizeof(input) - 1u, post) == ESP_ERR_NO_MEM);
    assert(rx.calls == 1u); /* Do not silently continue after a failed send. */
    rx = (receiver_t){.result = H2_PAL_ERR_TRUNCATED};
    assert(h2_esp_simcom_forward_urcs(&rx, input, sizeof(input) - 1u, post) == ESP_ERR_INVALID_SIZE);
    assert(rx.calls == 1u);
    rx = (receiver_t){0};
    uint8_t oversized[H2_SIMCOM_LINE_MAX + 8u];
    memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "RING\r\n", 6u);
    assert(h2_esp_simcom_forward_urcs(&rx, oversized, sizeof(oversized), post) == ESP_ERR_INVALID_SIZE);
    assert(rx.calls == 0u); /* No clipped suffix or partial packet delivery. */
    uint8_t maximum[H2_SIMCOM_LINE_MAX - 1u];
    memset(maximum, 'x', sizeof(maximum));
    assert(h2_esp_simcom_forward_urcs(&rx, maximum, sizeof(maximum), post) == ESP_OK);
    assert(strlen(rx.line) == sizeof(maximum));
    uint8_t several[400u];
    for (size_t i = 0u; i < sizeof(several); i += 2u) {
        several[i] = 'x'; several[i + 1u] = '\n';
    }
    rx = (receiver_t){0};
    assert(h2_esp_simcom_forward_urcs(&rx, several, sizeof(several), post) == ESP_OK);
    assert(rx.calls == 200u);
    assert(h2_esp_simcom_forward_urcs(&rx, NULL, 0u, post) == ESP_ERR_NOT_FOUND);
    return 0;
}
