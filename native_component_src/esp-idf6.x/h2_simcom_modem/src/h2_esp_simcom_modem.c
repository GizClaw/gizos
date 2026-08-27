#include "h2_esp_simcom_modem.h"
#include "h2_esp_simcom_teardown.h"

#include "h2_esp_platform_core.h"
#include "h2_simcom_modem.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_modem_c_api_types.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define H2_ESP_SIMCOM_GOT_IP_BIT BIT0
#define H2_ESP_SIMCOM_STOPPED_BIT BIT1
#define H2_ESP_SIMCOM_ERROR_BIT BIT2
#define H2_ESP_SIMCOM_GNSS_READY_BIT BIT3
#ifndef CONFIG_ESP_MODEM_C_API_STR_MAX
#define CONFIG_ESP_MODEM_C_API_STR_MAX 128
#endif

#ifdef CONFIG_ESP_MODEM_URC_HANDLER
static const char *TAG = "h2_simcom";
#endif

struct h2_esp_simcom_modem {
    h2_esp_simcom_modem_config_t config;
    h2_simcom_modem_t driver;
    esp_modem_dce_t *dce;
    esp_netif_t *ppp_netif;
    esp_netif_t *previous_default_netif;
    EventGroupHandle_t events;
    esp_event_handler_instance_t got_ip_instance;
    esp_event_handler_instance_t lost_ip_instance;
    esp_event_handler_instance_t ppp_status_instance;
    bool transport_ready;
    bool ppp_default_active;
    bool close_requested;
};

static h2_esp_simcom_modem_t *s_urc_modem;

static h2_pal_result_t transport_deinit(void *user);

static h2_pal_result_t map_esp_error(esp_err_t err) {
    switch (err) {
        case ESP_OK:
            return H2_PAL_OK;
        case ESP_ERR_INVALID_ARG:
            return H2_PAL_ERR_INVALID_ARG;
        case ESP_ERR_NO_MEM:
            return H2_PAL_ERR_NO_MEMORY;
        case ESP_ERR_TIMEOUT:
            return H2_PAL_ERR_TIMEOUT;
        case ESP_ERR_NOT_SUPPORTED:
            return H2_PAL_ERR_UNSUPPORTED;
        default:
            return H2_PAL_ERR_IO;
    }
}

static void set_data_closed(
    h2_esp_simcom_modem_t *modem,
    h2_pal_result_t last_error) {
    if (!modem->close_requested) {
        h2_simcom_modem_notify_data_closed(&modem->driver, last_error);
        return;
    }
    modem->driver.data_status.state = H2_PAL_MODEM_DATA_CLOSED;
    modem->driver.data_status.ip4 = 0u;
    modem->driver.data_status.dns1_ip4 = 0u;
    modem->driver.data_status.dns2_ip4 = 0u;
    modem->driver.data_status.ip4_valid = 0u;
    modem->driver.data_status.last_error = last_error;
}

static void restore_default_netif(h2_esp_simcom_modem_t *modem) {
    if (modem->ppp_default_active &&
        esp_netif_get_default_netif() == modem->ppp_netif) {
        if (modem->previous_default_netif != NULL) {
            (void)esp_netif_set_default_netif(modem->previous_default_netif);
        } else if (modem->ppp_netif != NULL) {
            esp_netif_action_disconnected(
                modem->ppp_netif,
                IP_EVENT,
                IP_EVENT_PPP_LOST_IP,
                NULL);
        }
    }
    modem->previous_default_netif = NULL;
    modem->ppp_default_active = false;
    (void)h2_esp_platform_netif_reconcile_default();
}

static void make_ppp_default(h2_esp_simcom_modem_t *modem) {
    esp_netif_t *current = esp_netif_get_default_netif();
    if (current == NULL) {
        /* GOT_IP already runs ESP-NETIF route selection. Do not create a
         * manual override that IDF v6.0 cannot clear with a NULL netif. */
        (void)h2_esp_platform_netif_reconcile_default();
        return;
    }
    if (current == modem->ppp_netif) {
        (void)h2_esp_platform_netif_reconcile_default();
        return;
    }
    modem->previous_default_netif = current;
    if (esp_netif_set_default_netif(modem->ppp_netif) == ESP_OK) {
        modem->ppp_default_active = true;
    }
    (void)h2_esp_platform_netif_reconcile_default();
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)base;
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)arg;
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        modem->driver.data_status.state = H2_PAL_MODEM_DATA_OPEN;
        modem->driver.data_status.ip4 = got_ip != NULL ? got_ip->ip_info.ip.addr : 0u;
        modem->driver.data_status.ip4_valid = got_ip != NULL && got_ip->ip_info.ip.addr != 0u;
        modem->driver.data_status.last_error = H2_PAL_OK;
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(modem->ppp_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            modem->driver.data_status.dns1_ip4 = dns.ip.u_addr.ip4.addr;
        }
        if (esp_netif_get_dns_info(modem->ppp_netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
            modem->driver.data_status.dns2_ip4 = dns.ip.u_addr.ip4.addr;
        }
        make_ppp_default(modem);
        xEventGroupSetBits(modem->events, H2_ESP_SIMCOM_GOT_IP_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        set_data_closed(
            modem,
            modem->close_requested ? H2_PAL_OK : H2_PAL_ERR_IO);
        restore_default_netif(modem);
        xEventGroupSetBits(modem->events, H2_ESP_SIMCOM_STOPPED_BIT);
    }
}

static void ppp_status_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)base;
    (void)event_data;
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)arg;
    if (event_id == NETIF_PPP_PHASE_DEAD || event_id == NETIF_PPP_PHASE_DISCONNECT) {
        set_data_closed(
            modem,
            modem->close_requested ? H2_PAL_OK : H2_PAL_ERR_IO);
        xEventGroupSetBits(modem->events, H2_ESP_SIMCOM_STOPPED_BIT);
        if (!modem->close_requested) {
            xEventGroupSetBits(modem->events, H2_ESP_SIMCOM_ERROR_BIT);
        }
        restore_default_netif(modem);
    } else if (event_id == NETIF_PPP_ERRORCONNECT ||
               event_id == NETIF_PPP_ERRORAUTHFAIL ||
               event_id == NETIF_PPP_ERRORPEERDEAD ||
               event_id == NETIF_PPP_CONNECT_FAILED) {
        set_data_closed(modem, H2_PAL_ERR_IO);
        xEventGroupSetBits(modem->events, H2_ESP_SIMCOM_ERROR_BIT);
    }
}

#ifdef CONFIG_ESP_MODEM_URC_HANDLER
static esp_err_t urc_handler(uint8_t *data, size_t len) {
    if (s_urc_modem == NULL || data == NULL || len == 0u) {
        return ESP_ERR_NOT_FOUND;
    }
    char line[H2_SIMCOM_LINE_MAX];
    size_t copy_len = len < sizeof(line) - 1u ? len : sizeof(line) - 1u;
    memcpy(line, data, copy_len);
    line[copy_len] = '\0';
    char *cursor = line;
    while (*cursor != '\0') {
        while (*cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        char *end = cursor;
        while (*end != '\0' && *end != '\r' && *end != '\n') {
            ++end;
        }
        char saved = *end;
        *end = '\0';
        h2_simcom_handle_urc_line(&s_urc_modem->driver, cursor);
        if (strncmp(
                cursor,
                "+CGNSSPWR: READY!",
                sizeof("+CGNSSPWR: READY!") - 1u) == 0 &&
            s_urc_modem->events != NULL) {
            xEventGroupSetBits(
                s_urc_modem->events, H2_ESP_SIMCOM_GNSS_READY_BIT);
        }
        if (saved == '\0') {
            break;
        }
        cursor = end + 1;
    }
    return ESP_OK;
}
#endif

static h2_pal_result_t set_power(h2_esp_simcom_modem_t *modem, int enabled) {
    if (modem->config.power_gpio < 0) {
        return H2_PAL_OK;
    }
    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << (unsigned)modem->config.power_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gpio_cfg);
    if (err == ESP_OK) {
        const int level = enabled ? modem->config.power_on_level : !modem->config.power_on_level;
        err = gpio_set_level((gpio_num_t)modem->config.power_gpio, level);
    }
    return map_esp_error(err);
}

static h2_pal_result_t transport_init(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (modem->transport_ready) {
        return H2_PAL_OK;
    }
    if (s_urc_modem != NULL && s_urc_modem != modem) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    h2_pal_result_t rc = set_power(modem, 1);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)set_power(modem, 0);
        return map_esp_error(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)set_power(modem, 0);
        return map_esp_error(err);
    }
    modem->events = xEventGroupCreate();
    if (modem->events == NULL) {
        (void)set_power(modem, 0);
        return H2_PAL_ERR_NO_MEMORY;
    }
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_PPP();
    modem->ppp_netif = esp_netif_new(&netif_config);
    if (modem->ppp_netif == NULL) {
        (void)transport_deinit(modem);
        return H2_PAL_ERR_NO_MEMORY;
    }
    h2_esp_platform_netif_register(
        modem->ppp_netif, H2_PAL_NETIF_KIND_MODEM_DATA);
    const esp_netif_ppp_config_t ppp_config = {
        .ppp_phase_event_enabled = true,
        .ppp_error_event_enabled = true,
    };
    err = esp_netif_ppp_set_params(modem->ppp_netif, &ppp_config);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_PPP_GOT_IP, ip_event_handler, modem, &modem->got_ip_instance);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_PPP_LOST_IP, ip_event_handler, modem, &modem->lost_ip_instance);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(
            NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, ppp_status_handler, modem, &modem->ppp_status_instance);
    }
    if (err != ESP_OK) {
        h2_pal_result_t rc = map_esp_error(err);
        (void)transport_deinit(modem);
        return rc;
    }
    const esp_modem_dte_config_t dte_config = {
        .dte_buffer_size = 2048u,
        .task_stack_size = 6144u,
        .task_priority = 8u,
        .uart_config = {
            .port_num = modem->config.uart_port,
            .data_bits = UART_DATA_8_BITS,
            .stop_bits = UART_STOP_BITS_1,
            .parity = UART_PARITY_DISABLE,
            .flow_control = ESP_MODEM_FLOW_CONTROL_NONE,
            .source_clk = ESP_MODEM_DEFAULT_UART_CLK,
            .baud_rate = (int)modem->config.baud_rate,
            .tx_io_num = modem->config.tx_gpio,
            .rx_io_num = modem->config.rx_gpio,
            .rts_io_num = modem->config.rts_gpio,
            .cts_io_num = modem->config.cts_gpio,
            .rx_buffer_size = 8192u,
            .tx_buffer_size = 4096u,
            .event_queue_size = 30,
        },
    };
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(
        modem->config.default_apn != NULL ? modem->config.default_apn : "internet");
    modem->dce = esp_modem_new(&dte_config, &dce_config, modem->ppp_netif);
    if (modem->dce == NULL) {
        (void)transport_deinit(modem);
        return H2_PAL_ERR_NO_MEMORY;
    }
#ifdef CONFIG_ESP_MODEM_URC_HANDLER
    s_urc_modem = modem;
    err = esp_modem_set_urc(modem->dce, urc_handler);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "URC handler unavailable: %s", esp_err_to_name(err));
    }
#endif
    modem->transport_ready = true;
    return H2_PAL_OK;
}

static h2_pal_result_t teardown_power_off(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    return set_power(modem, 0);
}

static h2_pal_result_t teardown_restore_default_netif(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    restore_default_netif(modem);
    return H2_PAL_OK;
}

static h2_pal_result_t teardown_unregister_event_handlers(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    esp_err_t err;
    /* Instance unregistration synchronizes on the ESP event-loop mutex. Once
     * all three calls return, no active handler can still use modem->events. */
    if (modem->ppp_status_instance != NULL) {
        err = esp_event_handler_instance_unregister(
            NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, modem->ppp_status_instance);
        if (err != ESP_OK) {
            return map_esp_error(err);
        }
        modem->ppp_status_instance = NULL;
    }
    if (modem->lost_ip_instance != NULL) {
        err = esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_PPP_LOST_IP, modem->lost_ip_instance);
        if (err != ESP_OK) {
            return map_esp_error(err);
        }
        modem->lost_ip_instance = NULL;
    }
    if (modem->got_ip_instance != NULL) {
        err = esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_PPP_GOT_IP, modem->got_ip_instance);
        if (err != ESP_OK) {
            return map_esp_error(err);
        }
        modem->got_ip_instance = NULL;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t teardown_destroy_dce(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
#ifdef CONFIG_ESP_MODEM_URC_HANDLER
    if (modem->dce != NULL) {
        (void)esp_modem_set_urc(modem->dce, NULL);
    }
    if (s_urc_modem == modem) {
        s_urc_modem = NULL;
    }
#endif
    if (modem->dce != NULL) {
        esp_modem_destroy(modem->dce);
        modem->dce = NULL;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t teardown_destroy_netif(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (modem->ppp_netif != NULL) {
        h2_esp_platform_netif_unregister(modem->ppp_netif);
        esp_netif_destroy(modem->ppp_netif);
        modem->ppp_netif = NULL;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t teardown_destroy_event_group(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (modem->events != NULL) {
        vEventGroupDelete(modem->events);
        modem->events = NULL;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t transport_deinit(void *user) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    modem->close_requested = true;
    const h2_esp_simcom_teardown_config_t config = {
        .user = modem,
        .power_off = teardown_power_off,
        .restore_default_netif = teardown_restore_default_netif,
        .unregister_event_handlers = teardown_unregister_event_handlers,
        .destroy_dce = teardown_destroy_dce,
        .destroy_netif = teardown_destroy_netif,
        .destroy_event_group = teardown_destroy_event_group,
    };
    const h2_pal_result_t rc = h2_esp_simcom_run_teardown(&config);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    modem->transport_ready = false;
    modem->close_requested = false;
    return H2_PAL_OK;
}

static h2_pal_result_t transport_command(
    void *user,
    const char *cmd,
    char *response,
    size_t response_size,
    uint32_t timeout_ms) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (cmd == NULL || response == NULL ||
        response_size < (size_t)CONFIG_ESP_MODEM_C_API_STR_MAX) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = transport_init(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    if (strncmp(
            cmd,
            "AT+CGNSSPWR=",
            sizeof("AT+CGNSSPWR=") - 1u) == 0) {
        xEventGroupClearBits(modem->events, H2_ESP_SIMCOM_GNSS_READY_BIT);
    }
    response[0] = '\0';
    const esp_err_t err = esp_modem_at(
        modem->dce, cmd, response, (int)timeout_ms);
    if (err == ESP_OK && strcmp(cmd, "AT+CGNSSPWR=1") == 0 &&
        strstr(response, "+CGNSSPWR: READY!") != NULL) {
        xEventGroupSetBits(modem->events, H2_ESP_SIMCOM_GNSS_READY_BIT);
    }
    return map_esp_error(err);
}

static h2_pal_result_t transport_wait_gnss_ready(
    void *user,
    uint32_t timeout_ms) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (modem == NULL || modem->events == NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    const TickType_t ticks = timeout_ms == UINT32_MAX
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        modem->events,
        H2_ESP_SIMCOM_GNSS_READY_BIT,
        pdTRUE,
        pdFALSE,
        ticks);
    return (bits & H2_ESP_SIMCOM_GNSS_READY_BIT) != 0u
        ? H2_PAL_OK
        : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t transport_data_open(
    void *user,
    uint32_t timeout_ms,
    h2_pal_modem_data_status_t *out_status) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (out_status == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_result_t rc = transport_init(modem);
    if (rc != H2_PAL_OK) {
        return rc;
    }
    const char *apn = modem->driver.last_apn[0] != '\0'
        ? modem->driver.last_apn
        : modem->config.default_apn;
    if (apn == NULL || apn[0] == '\0') {
        apn = "internet";
    }
    esp_err_t err = esp_modem_set_apn(modem->dce, apn);
    if (err != ESP_OK) {
        return map_esp_error(err);
    }
    esp_netif_auth_type_t auth = NETIF_PPP_AUTHTYPE_NONE;
    if (modem->driver.last_username[0] != '\0' || modem->driver.last_password[0] != '\0') {
        auth = (esp_netif_auth_type_t)(NETIF_PPP_AUTHTYPE_PAP | NETIF_PPP_AUTHTYPE_CHAP);
    }
    err = esp_netif_ppp_set_auth(
        modem->ppp_netif, auth, modem->driver.last_username, modem->driver.last_password);
    if (err != ESP_OK) {
        return map_esp_error(err);
    }
    modem->close_requested = false;
    xEventGroupClearBits(
        modem->events,
        H2_ESP_SIMCOM_GOT_IP_BIT | H2_ESP_SIMCOM_STOPPED_BIT | H2_ESP_SIMCOM_ERROR_BIT);
    err = esp_modem_set_mode(modem->dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        return map_esp_error(err);
    }
    const EventBits_t bits = xEventGroupWaitBits(
        modem->events,
        H2_ESP_SIMCOM_GOT_IP_BIT | H2_ESP_SIMCOM_ERROR_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms == 0u ? 45000u : timeout_ms));
    if ((bits & H2_ESP_SIMCOM_ERROR_BIT) != 0u ||
        (bits & H2_ESP_SIMCOM_GOT_IP_BIT) == 0u) {
        const esp_err_t rollback_err = esp_modem_set_mode(
            modem->dce, ESP_MODEM_MODE_COMMAND);
        restore_default_netif(modem);
        if (rollback_err != ESP_OK) {
            const h2_pal_result_t rollback_rc = map_esp_error(rollback_err);
            (void)transport_deinit(modem);
            return rollback_rc;
        }
        return (bits & H2_ESP_SIMCOM_ERROR_BIT) != 0u
            ? H2_PAL_ERR_IO
            : H2_PAL_ERR_TIMEOUT;
    }
    *out_status = modem->driver.data_status;
    return H2_PAL_OK;
}

static h2_pal_result_t transport_data_close(void *user, uint32_t timeout_ms) {
    h2_esp_simcom_modem_t *modem = (h2_esp_simcom_modem_t *)user;
    if (modem->dce == NULL) {
        restore_default_netif(modem);
        return H2_PAL_OK;
    }
    modem->close_requested = true;
    xEventGroupClearBits(modem->events, H2_ESP_SIMCOM_STOPPED_BIT);
    esp_err_t err = esp_modem_set_mode(modem->dce, ESP_MODEM_MODE_COMMAND);
    bool stopped = false;
    if (err == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(
            modem->events,
            H2_ESP_SIMCOM_STOPPED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(timeout_ms == 0u ? 2000u : timeout_ms));
        stopped = (bits & H2_ESP_SIMCOM_STOPPED_BIT) != 0u;
    }
    restore_default_netif(modem);
    modem->close_requested = false;
    if (err != ESP_OK) {
        return map_esp_error(err);
    }
    return stopped ? H2_PAL_OK : H2_PAL_ERR_TIMEOUT;
}

h2_pal_result_t h2_esp_simcom_modem_create(
    const h2_esp_simcom_modem_config_t *config,
    h2_esp_simcom_modem_t **out_modem) {
    if (config == NULL || out_modem == NULL || config->uart_port < 0 ||
        config->uart_port >= UART_NUM_MAX ||
        config->tx_gpio < 0 || config->tx_gpio >= GPIO_NUM_MAX ||
        config->rx_gpio < 0 || config->rx_gpio >= GPIO_NUM_MAX ||
        config->rts_gpio < -1 || config->rts_gpio >= GPIO_NUM_MAX ||
        config->cts_gpio < -1 || config->cts_gpio >= GPIO_NUM_MAX ||
        config->tx_gpio == config->rx_gpio || config->power_gpio < -1 ||
        config->power_gpio >= GPIO_NUM_MAX ||
        (config->power_on_level != 0 && config->power_on_level != 1) ||
        config->baud_rate == 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    *out_modem = NULL;
    h2_esp_simcom_modem_t *modem = calloc(1u, sizeof(*modem));
    if (modem == NULL) {
        return H2_PAL_ERR_NO_MEMORY;
    }
    modem->config = *config;
    const h2_simcom_modem_config_t driver_config = {
        .transport_user = modem,
        .init = transport_init,
        .deinit = transport_deinit,
        .command = transport_command,
        .data_open = transport_data_open,
        .data_close = transport_data_close,
        .wait_gnss_ready = transport_wait_gnss_ready,
        .sync_api = config->sync_api,
        .allocator = config->allocator,
        .system_events = config->system_events,
        .capabilities = H2_PAL_MODEM_CAPABILITY_DATA |
                        H2_PAL_MODEM_CAPABILITY_CALL |
                        H2_PAL_MODEM_CAPABILITY_GNSS,
        .command_timeout_ms = 5000u,
        .io_timeout_ms = 250u,
    };
    h2_pal_result_t rc = h2_simcom_modem_init(&modem->driver, &driver_config);
    if (rc != H2_PAL_OK) {
        free(modem);
        return rc;
    }
    *out_modem = modem;
    return H2_PAL_OK;
}

void h2_esp_simcom_modem_destroy(h2_esp_simcom_modem_t *modem) {
    if (modem == NULL) {
        return;
    }
    h2_simcom_modem_deinit(&modem->driver);
    if (modem->transport_ready) {
        (void)transport_deinit(modem);
    }
    free(modem);
}

h2_pal_modem_api_t *h2_esp_simcom_modem_api(h2_esp_simcom_modem_t *modem) {
    return modem != NULL ? h2_simcom_modem_platform(&modem->driver) : NULL;
}
