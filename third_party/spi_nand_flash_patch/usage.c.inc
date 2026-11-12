#include "h2_spi_nand_flash_usage.h"

esp_err_t h2_spi_nand_flash_get_usage(spi_nand_flash_device_t *handle,
                                    uint64_t *used_bytes, uint64_t *total_bytes)
{
    if (!handle || !used_bytes || !total_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    *used_bytes = 0;
    *total_bytes = 0;
    if (!handle->mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (handle->ops && handle->ops->read == dhara_read && handle->ops_priv_data) {
        const spi_nand_flash_dhara_priv_data_t *state = handle->ops_priv_data;
        const uint64_t used = dhara_map_size(&state->dhara_map);
        const uint64_t total = dhara_map_capacity(&state->dhara_map);
        if (total && used <= total && handle->chip.page_size) {
            *used_bytes = used * handle->chip.page_size;
            *total_bytes = total * handle->chip.page_size;
            result = ESP_OK;
        }
    }
    if (xSemaphoreGive(handle->mutex) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    return result;
}
