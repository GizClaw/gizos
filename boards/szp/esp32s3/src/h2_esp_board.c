#include "h2_esp_board_private.h"

#include "h2_esp_board_config.h"
#include "h2_esp_szp_board_internal.h"
#include "h2_esp_platform_core.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"

#define H2_SZP_I2C_PORT 0
#define H2_SZP_I2C_SDA_GPIO 1
#define H2_SZP_I2C_SCL_GPIO 2
#define H2_SZP_I2C_SPEED_HZ 100000u
#define H2_SZP_PCA9557_ADDR 0x19u
#define H2_SZP_PCA9557_REG_OUTPUT 0x01u
#define H2_SZP_PCA9557_REG_CONFIG 0x03u
#define H2_SZP_PCA_LCD_CS_MASK (1u << 0)
#define H2_SZP_PCA_PA_MASK (1u << 1)
#define H2_SZP_PCA_DVP_PWDN_MASK (1u << 2)
#define H2_SZP_PCA_OUTPUT_MASK (H2_SZP_PCA_LCD_CS_MASK | H2_SZP_PCA_PA_MASK | H2_SZP_PCA_DVP_PWDN_MASK)
#define H2_SZP_PCA_INITIAL_OUTPUT (H2_SZP_PCA_LCD_CS_MASK | H2_SZP_PCA_DVP_PWDN_MASK)

static const char *TAG = "h2_esp_szp";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_pca9557;
static uint8_t s_pca_output = H2_SZP_PCA_INITIAL_OUTPUT;

static int esp_rc(esp_err_t err) {
    return err == ESP_OK ? 0 : -1;
}

static esp_err_t pca_write(uint8_t reg, uint8_t value) {
    const uint8_t data[2] = { reg, value };
    return i2c_master_transmit(s_pca9557, data, sizeof(data), pdMS_TO_TICKS(100));
}

int h2_esp_szp_board_init_io(void) {
    if (s_pca9557 != NULL) {
        return 0;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = H2_SZP_I2C_PORT,
        .sda_io_num = H2_SZP_I2C_SDA_GPIO,
        .scl_io_num = H2_SZP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        err = i2c_master_get_bus_handle(H2_SZP_I2C_PORT, &s_i2c_bus);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(err));
        return -1;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = H2_SZP_PCA9557_ADDR,
        .scl_speed_hz = H2_SZP_I2C_SPEED_HZ,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_pca9557);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pca9557 add failed: %s", esp_err_to_name(err));
        return -1;
    }

    s_pca_output = H2_SZP_PCA_INITIAL_OUTPUT;
    err = pca_write(H2_SZP_PCA9557_REG_OUTPUT, s_pca_output);
    if (err == ESP_OK) {
        err = pca_write(H2_SZP_PCA9557_REG_CONFIG, (uint8_t)~H2_SZP_PCA_OUTPUT_MASK);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pca9557 init failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

i2c_master_bus_handle_t h2_esp_szp_board_i2c_bus(void) {
    if (h2_esp_szp_board_init_io() != 0) {
        return NULL;
    }
    return s_i2c_bus;
}

static int set_pca_mask(uint8_t mask, int high) {
    if (h2_esp_szp_board_init_io() != 0) {
        return -1;
    }
    if (high) {
        s_pca_output = (uint8_t)(s_pca_output | mask);
    } else {
        s_pca_output = (uint8_t)(s_pca_output & (uint8_t)~mask);
    }
    return esp_rc(pca_write(H2_SZP_PCA9557_REG_OUTPUT, s_pca_output));
}

int h2_esp_szp_board_set_lcd_cs(int high) {
    return set_pca_mask(H2_SZP_PCA_LCD_CS_MASK, high);
}

int h2_esp_szp_board_set_pa(int enabled) {
    return set_pca_mask(H2_SZP_PCA_PA_MASK, enabled);
}

int h2_esp_board_h2loader_fs_init(h2_pal_fs_api_t *fs);
int h2_esp_board_h2loader_fs_deinit(void);

int h2_esp_board_fs_init(h2_pal_fs_api_t *fs) {
    uint8_t partition_subtype;
    const h2_esp_platform_spiffs_config_t config = {
        .base_path = H2_ESP_BOARD_FS_BASE_PATH,
        .partition_label = H2_ESP_BOARD_FS_PARTITION_LABEL,
        .max_files = 16u,
        .format_if_mount_failed = true,
    };

    h2_pal_result_t rc = h2_esp_platform_data_partition_subtype(
        H2_ESP_BOARD_FS_PARTITION_LABEL, &partition_subtype);
    if (rc == H2_PAL_OK) {
        return h2_esp_platform_spiffs_fs_init(fs, &config);
    }
    if (rc != H2_PAL_ERR_NOT_FOUND) {
        return rc;
    }
    return h2_esp_board_h2loader_fs_init(fs);
}

int h2_esp_board_fs_deinit(void) {
    int legacy_rc = h2_esp_platform_spiffs_fs_deinit(H2_ESP_BOARD_FS_PARTITION_LABEL);
    int h2loader_rc = h2_esp_board_h2loader_fs_deinit();

    if (legacy_rc == H2_PAL_FS_OK || h2loader_rc == H2_PAL_FS_OK) {
        return H2_PAL_FS_OK;
    }
    return h2loader_rc;
}

h2_pal_mem_api_t *h2_esp_board_default_allocator(void) {
    return h2_esp_platform_default_allocator();
}

h2_pal_mem_api_t *h2_esp_board_psram_allocator(void) {
    return h2_esp_platform_psram_allocator();
}

h2_pal_mem_api_t *h2_esp_board_internal_allocator(void) {
    return h2_esp_platform_internal_allocator();
}

h2_pal_mem_api_t *h2_esp_board_dma_allocator(void) {
    return h2_esp_platform_dma_allocator();
}

const h2_pal_log_api_t *h2_esp_board_log_api(void) {
    return h2_esp_platform_log_api();
}

const h2_pal_sync_api_t *h2_esp_board_sync_api(void) {
    return h2_esp_platform_sync_api();
}

const h2_pal_task_api_t *h2_esp_board_task_api(void) {
    return h2_esp_platform_task_api();
}

const h2_pal_queue_api_t *h2_esp_board_queue_api(void) {
    return h2_esp_platform_queue_api();
}

const h2_pal_time_api_t *h2_esp_board_time_api(void) {
    return h2_esp_platform_time_api();
}

const h2_pal_system_event_api_t *h2_esp_board_system_event_api(void) {
    return h2_esp_platform_system_event_api();
}

const h2_pal_crypto_api_t *h2_esp_board_crypto_api(void) {
    return h2_esp_platform_crypto_api();
}

const h2_pal_disk_api_t *h2_esp_board_disk_api(void) {
    return h2_esp_platform_disk_api();
}

const h2_pal_pref_api_t *h2_esp_board_pref_api(void) {
    return h2_esp_platform_pref_api();
}

const h2_pal_webrtc_api_t *h2_esp_board_webrtc_api(void) {
    return h2_esp_platform_webrtc_api();
}

h2_pal_ble_t *h2_esp_board_ble(void) {
    return h2_esp_platform_ble();
}

h2_pal_wifi_sta_t *h2_esp_board_wifi_sta(void) {
    return h2_esp_platform_wifi_sta();
}

h2_pal_wifi_ap_t *h2_esp_board_wifi_ap(void) {
    return h2_esp_platform_wifi_ap();
}

h2_pal_wifi_settings_t *h2_esp_board_wifi_settings(void) {
    return h2_esp_platform_wifi_settings();
}

h2_pal_modem_t *h2_esp_board_modem(void) {
    return h2_esp_platform_modem_unsupported();
}
