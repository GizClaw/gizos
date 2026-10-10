#include "h2_chip_board.h"

#include "esp_efuse.h"
#include "esp_efuse_table.h"

#include <stddef.h>

const char *h2_chip_board_name(void) { return "esp_mosaico"; }

int h2_mosaico_board_revision(h2_mosaico_revision_t *out_revision) {
    (void)h2_mosaico_revision_decode(NULL, out_revision);
    if (out_revision == NULL) {
        return -1;
    }
    uint8_t bytes[2] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, bytes, 16) != ESP_OK) {
        return -1;
    }
    return h2_mosaico_revision_decode(bytes, out_revision);
}
