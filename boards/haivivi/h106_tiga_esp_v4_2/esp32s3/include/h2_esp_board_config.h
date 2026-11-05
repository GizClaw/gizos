#ifndef H2_ESP_BOARD_CONFIG_H
#define H2_ESP_BOARD_CONFIG_H

/* Board-owned PSRAM reservation, before any service or UI initialization.
 * Keep the system reserve out of total free PSRAM (IDF, network stacks,
 * non-PAL tasks), then limit the arena by the largest block and this cap. */
#ifndef H2_TIGA_ESP_ARENA_BYTES
#define H2_TIGA_ESP_ARENA_BYTES (4u * 1024u * 1024u)
#endif
#ifndef H2_TIGA_ESP_ARENA_SYSTEM_RESERVE_BYTES
#define H2_TIGA_ESP_ARENA_SYSTEM_RESERVE_BYTES (768u * 1024u)
#endif
#ifndef H2_TIGA_ESP_ARENA_SMALL_REQUEST_MAX
#define H2_TIGA_ESP_ARENA_SMALL_REQUEST_MAX (32u * 1024u)
#endif
#ifndef H2_TIGA_ESP_ARENA_SMALL_POOL_BYTES
#define H2_TIGA_ESP_ARENA_SMALL_POOL_BYTES (1024u * 1024u)
#endif

#define H2_ESP_BOARD_NAME "tiga_esp_v4_2"
#define H2_ESP_BOARD_FS_BASE_PATH "/h2fs"
#define H2_ESP_BOARD_FS_PARTITION_LABEL "h2test"
#define H2_ESP_BOARD_HAS_LED_STRIP 1
#define H2_ESP_BOARD_HAS_NFC 1
#define H2_TIGA_ESP_ADC_BUTTON_GROUP_ID 100u
#define H2_TIGA_ESP_BOOT_BUTTON_ID 101u
#define H2_TIGA_ESP_BUTTON_OK_ID 102u
#define H2_TIGA_ESP_BUTTON_LEFT_ID 103u
#define H2_TIGA_ESP_BUTTON_RIGHT_ID 104u
#define H2_TIGA_ESP_BUTTON_CANCEL_ID 105u
#define H2_TIGA_ESP_BUTTON_VOL_INC_ID 106u
#define H2_TIGA_ESP_BUTTON_VOL_DEC_ID 107u
#define H2_TIGA_ESP_BUTTON_RECORD_ID 108u
#define H2_TIGA_ESP_NFC_READER_ID 201u
#define H2_TIGA_ESP_MODEM_RI_ID 301u
#define H2_TIGA_ESP_BATTERY_ID 401u
#define H2_TIGA_ESP_SOC_TEMPERATURE_ID 501u
#define H2_TIGA_ESP_LED_STRIP_ID 601u
#define H2_TIGA_ESP_VIBRATION_ID 701u

#define H2_TIGA_ESP_POWER_HOLD_GPIO 15u
#define H2_TIGA_ESP_BOOT_BUTTON_GPIO 21u
#define H2_TIGA_ESP_CHARGE_DETECT_GPIO 13u
#define H2_TIGA_ESP_MODEM_RI_GPIO 0u
/* Hardware README section 1.5: GPIO6 high = DTR low (awake); low = sleep. */
#define H2_TIGA_ESP_MODEM_DTR_INVERTED_GPIO 6u
#define H2_TIGA_ESP_AUDIO_MODEM_ROUTE_GPIO 38u
#define H2_TIGA_ESP_AUDIO_MODEM_ROUTE_CODEC_LEVEL 0u
#define H2_TIGA_ESP_AUDIO_MODEM_ROUTE_CALL_LEVEL 1u
#define H2_TIGA_ESP_AUDIO_PA_GPIO 14u
/* Shared I2C0, SDA GPIO2 / SCL GPIO1; addresses are 7-bit. */
#define H2_TIGA_ESP_AUDIO_ES8311_ADDR 0x18u
#define H2_TIGA_ESP_NFC_I2C_ADDR 0x28u
#define H2_TIGA_ESP_PERIPHERAL_I2C_SPEED_HZ 100000u
#define H2_TIGA_ESP_VIBRATION_GPIO 7u
#define H2_TIGA_ESP_VIBRATION_LEDC_CHANNEL 1u
#define H2_TIGA_ESP_VIBRATION_LEDC_TIMER 1u
#define H2_TIGA_ESP_VIBRATION_FREQUENCY_HZ 5000u
#define H2_TIGA_ESP_VIBRATION_DUTY_RESOLUTION_BITS 13u
#define H2_TIGA_ESP_LED_STRIP_GPIO 8u
#define H2_TIGA_ESP_LED_STRIP_PIXEL_COUNT 8u
#define H2_TIGA_ESP_LED_STRIP_CHANNELS_PER_PIXEL 3u
/* 100 ns ticks represent WS2812's 300/600/900 ns timings exactly. */
#define H2_TIGA_ESP_LED_STRIP_RMT_RES_HZ 10000000u
/* DMA keeps LED transmission independent of interrupt refill latency. */
#define H2_TIGA_ESP_LED_STRIP_RMT_DMA 1
#define H2_TIGA_ESP_EXTFLASH_CS_GPIO 5u
#define H2_TIGA_ESP_EXTFLASH_CLK_GPIO 4u
#define H2_TIGA_ESP_EXTFLASH_MOSI_GPIO 9u
#define H2_TIGA_ESP_EXTFLASH_MISO_GPIO 11u
#define H2_TIGA_ESP_EXTFLASH_FREQUENCY_MHZ 40u

/* QuecLocator evaluation token, shared by every H2Vivi product. Quectel's
 * free test tier: it is not billed and the service answers with fabricated
 * coordinates, so it grants no access to production data and is not a secret
 * under the root AGENTS.md credential rule. A production token would not be
 * committed. Still kept out of logs, traces and telemetry; the provider only
 * sends it to the module on the first locate call. An empty value leaves cell
 * locate unassembled. */
#define H2_TIGA_ESP_MODEM_CELL_LOCATE_TOKEN "W9H3E1A6564c6859"

#endif
