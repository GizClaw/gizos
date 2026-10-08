#include "h2_esp_lierda_modem.h"
#include "sdkconfig.h"

#include "h2_esp_platform_core.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_modem_api.h"
#include "esp_modem_c_api_types.h"
#include "esp_modem_config.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#define LIERDA_IP BIT0
#define LIERDA_STOP BIT1
#define LIERDA_ERROR BIT2
#ifndef CONFIG_ESP_MODEM_C_API_STR_MAX
#define CONFIG_ESP_MODEM_C_API_STR_MAX 128
#endif

struct h2_esp_lierda_modem {
    h2_esp_lierda_modem_config_t config;
    h2_lierda_modem_t *provider;
    h2_pal_mutex_t *mutex;
    esp_modem_dce_t *dce;
    esp_netif_t *netif;
    EventGroupHandle_t events;
    esp_event_handler_instance_t got_ip;
    esp_event_handler_instance_t lost_ip;
    esp_event_handler_instance_t ppp_status;
    bool powered;
    bool data_mode;
    bool command_confirmed;
    bool closing;
    bool initialized;
    h2_pal_modem_data_status_t data;
};

static h2_pal_result_t map_error(esp_err_t error) {
    switch (error) {
        case ESP_OK: return H2_PAL_OK;
        case ESP_ERR_NO_MEM: return H2_PAL_ERR_NO_MEMORY;
        case ESP_ERR_INVALID_ARG: return H2_PAL_ERR_INVALID_ARG;
        case ESP_ERR_INVALID_STATE: return H2_PAL_ERR_INVALID_STATE;
        case ESP_ERR_TIMEOUT: return H2_PAL_ERR_TIMEOUT;
        case ESP_ERR_NOT_SUPPORTED: return H2_PAL_ERR_UNSUPPORTED;
        default: return H2_PAL_ERR_IO;
    }
}

static h2_pal_result_t lock(h2_esp_lierda_modem_t *modem) {
    return h2_pal_mutex_lock(modem->config.sync_api, modem->mutex);
}

static h2_pal_result_t unlock(h2_esp_lierda_modem_t *modem, h2_pal_result_t rc) {
    const h2_pal_result_t result = h2_pal_mutex_unlock(modem->config.sync_api, modem->mutex);
    return rc != H2_PAL_OK ? rc : result;
}

static h2_pal_result_t data_failure(h2_esp_lierda_modem_t *modem, h2_pal_result_t rc) {
    if (lock(modem) == H2_PAL_OK) {
        if (modem->data_mode) {
            /* A failed/cancelled attempt cannot be revived by later link-up.
             * UART ownership remains fenced until COMMAND/whole close. */
            modem->closing = true;
            modem->data = (h2_pal_modem_data_status_t){
                .state = H2_PAL_MODEM_DATA_CLOSING, .last_error = rc,
            };
        } else modem->data.last_error = rc;
        (void)unlock(modem, H2_PAL_OK);
    }
    return rc;
}

static void ip_event(void *user, esp_event_base_t base, int32_t id, void *event_data) {
    (void)base;
    h2_esp_lierda_modem_t *modem = user;
    const ip_event_got_ip_t *event = event_data;
    if (event == NULL || event->esp_netif != modem->netif) return;
    if (id == IP_EVENT_PPP_GOT_IP) {
        esp_netif_ip_info_t current = {0};
        /* SDK current IP, cleared at each COMMAND boundary, rejects a queued
         * old GOT_IP before the next link actually acquires an address. */
        if (esp_netif_get_ip_info(modem->netif, &current) != ESP_OK ||
            current.ip.addr == 0u || current.ip.addr != event->ip_info.ip.addr) return;
        esp_netif_dns_info_t primary = {0}, secondary = {0};
        (void)esp_netif_get_dns_info(modem->netif, ESP_NETIF_DNS_MAIN, &primary);
        (void)esp_netif_get_dns_info(modem->netif, ESP_NETIF_DNS_BACKUP, &secondary);
        if (lock(modem) != H2_PAL_OK) return;
        if (modem->data_mode && !modem->closing) {
            modem->data = (h2_pal_modem_data_status_t){
                .state = H2_PAL_MODEM_DATA_OPEN,
                .ip4 = current.ip.addr, .ip4_valid = 1u,
                .dns1_ip4 = primary.ip.type == ESP_IPADDR_TYPE_V4
                    ? primary.ip.u_addr.ip4.addr : 0u,
                .dns2_ip4 = secondary.ip.type == ESP_IPADDR_TYPE_V4
                    ? secondary.ip.u_addr.ip4.addr : 0u,
            };
            xEventGroupSetBits(modem->events, LIERDA_IP);
        }
        (void)unlock(modem, H2_PAL_OK);
        (void)h2_esp_platform_netif_reconcile_default();
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        if (lock(modem) != H2_PAL_OK) return;
        if (modem->data_mode) {
            modem->data = (h2_pal_modem_data_status_t){
                .state = H2_PAL_MODEM_DATA_CLOSING,
                .last_error = modem->closing ? H2_PAL_OK : H2_PAL_ERR_IO,
            };
            /* Losing IP does not prove that the UART has returned to AT. */
            xEventGroupSetBits(modem->events, LIERDA_ERROR);
        }
        (void)unlock(modem, H2_PAL_OK);
        (void)h2_esp_platform_netif_reconcile_default();
    }
}

static void ppp_event(void *user, esp_event_base_t base, int32_t id, void *event_data) {
    (void)base;
    h2_esp_lierda_modem_t *modem = user;
    /* IDF 6.0.3 normal phase/error events carry esp_netif_t*. Its exceptional
     * CONNECT_FAILED producer has a different payload; mode return/timeout
     * handles that path without interpreting unqualified bytes. */
    if (id == NETIF_PPP_CONNECT_FAILED || event_data == NULL ||
        *(esp_netif_t **)event_data != modem->netif) return;
    if (lock(modem) != H2_PAL_OK) return;
    if (id == NETIF_PPP_PHASE_DEAD || id == NETIF_PPP_PHASE_DISCONNECT) {
        xEventGroupSetBits(modem->events, LIERDA_STOP);
        if (modem->data_mode) {
            modem->data = (h2_pal_modem_data_status_t){
                .state = H2_PAL_MODEM_DATA_CLOSING,
                .last_error = modem->closing ? H2_PAL_OK : H2_PAL_ERR_IO,
            };
            if (!modem->closing) xEventGroupSetBits(modem->events, LIERDA_ERROR);
        }
    } else if (modem->data_mode && (id == NETIF_PPP_ERRORCONNECT ||
               id == NETIF_PPP_ERRORAUTHFAIL || id == NETIF_PPP_ERRORPEERDEAD)) {
        modem->data = (h2_pal_modem_data_status_t){
            .state = H2_PAL_MODEM_DATA_CLOSING, .last_error = H2_PAL_ERR_IO,
        };
        xEventGroupSetBits(modem->events, LIERDA_ERROR);
    }
    (void)unlock(modem, H2_PAL_OK);
    (void)h2_esp_platform_netif_reconcile_default();
}

static h2_pal_result_t remove_handler(
    esp_event_base_t base, int32_t id, esp_event_handler_instance_t *instance) {
    if (*instance == NULL) return H2_PAL_OK;
    const esp_err_t error = esp_event_handler_instance_unregister(base, id, *instance);
    if (error == ESP_OK) *instance = NULL;
    return map_error(error);
}

static h2_pal_result_t transport_close(void *user, uint32_t timeout_ms) {
    h2_esp_lierda_modem_t *modem = user;
    h2_pal_result_t rc = lock(modem);
    if (rc != H2_PAL_OK) return rc;
    modem->closing = true;
    modem->data = (h2_pal_modem_data_status_t){.state = H2_PAL_MODEM_DATA_CLOSING};
    rc = unlock(modem, H2_PAL_OK);
    if (rc != H2_PAL_OK) return rc;
    if (modem->powered) {
        rc = modem->config.power(modem->config.power_user, 0, timeout_ms);
        if (rc != H2_PAL_OK) return rc;
        modem->powered = false;
    }
    /* Default-loop unregister synchronizes with an active handler. Stop on
     * any failure, retaining all resources still referenced by its callback. */
    rc = remove_handler(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &modem->ppp_status);
    if (rc == H2_PAL_OK) rc = remove_handler(IP_EVENT, IP_EVENT_PPP_LOST_IP, &modem->lost_ip);
    if (rc == H2_PAL_OK) rc = remove_handler(IP_EVENT, IP_EVENT_PPP_GOT_IP, &modem->got_ip);
    if (rc != H2_PAL_OK) return rc;
    if (modem->dce != NULL) {
        esp_modem_destroy(modem->dce);
        modem->dce = NULL;
    }
    if (modem->netif != NULL) {
        h2_esp_platform_netif_unregister(modem->netif);
        /* SDK list removal/reselection owns default-route cleanup. Never
         * retain or dereference an unrelated Wi-Fi netif as a restore token. */
        esp_netif_destroy(modem->netif);
        modem->netif = NULL;
    }
    if (modem->events != NULL) {
        vEventGroupDelete(modem->events);
        modem->events = NULL;
    }
    rc = lock(modem);
    if (rc != H2_PAL_OK) return rc;
    modem->data_mode = false;
    modem->command_confirmed = true;
    modem->closing = false;
    memset(&modem->data, 0, sizeof(modem->data));
    rc = unlock(modem, H2_PAL_OK);
    if (rc != H2_PAL_OK) return rc;
    return h2_esp_platform_netif_reconcile_default();
}

static h2_pal_result_t transport_open(void *user, uint32_t timeout_ms) {
    h2_esp_lierda_modem_t *modem = user;
    if (modem->dce != NULL || modem->powered || modem->netif != NULL) {
        return H2_PAL_ERR_INVALID_STATE;
    }
    modem->powered = true; /* retain even partially failed board power-on */
    h2_pal_result_t rc = modem->config.power(modem->config.power_user, 1, timeout_ms);
    if (rc != H2_PAL_OK) return rc;
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return map_error(error);
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return map_error(error);
    modem->events = xEventGroupCreate();
    if (modem->events == NULL) return H2_PAL_ERR_NO_MEMORY;
    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_PPP();
    modem->netif = esp_netif_new(&netif_config);
    if (modem->netif == NULL) return H2_PAL_ERR_NO_MEMORY;
    h2_esp_platform_netif_register(modem->netif, H2_PAL_NETIF_KIND_MODEM_DATA);
    const esp_netif_ppp_config_t ppp_config = {
        .ppp_phase_event_enabled = true, .ppp_error_event_enabled = true,
    };
    error = esp_netif_ppp_set_params(modem->netif, &ppp_config);
    if (error == ESP_OK) error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_PPP_GOT_IP, ip_event, modem, &modem->got_ip);
    if (error == ESP_OK) error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_PPP_LOST_IP, ip_event, modem, &modem->lost_ip);
    if (error == ESP_OK) error = esp_event_handler_instance_register(
        NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, ppp_event, modem, &modem->ppp_status);
    if (error != ESP_OK) return map_error(error);
    const esp_modem_dte_config_t dte = {
        .dte_buffer_size = 2048u,
        .task_stack_size = modem->config.dte_stack_size != 0u ? modem->config.dte_stack_size : 8192u,
        .task_priority = modem->config.dte_priority != 0u ? modem->config.dte_priority : 8u,
        .uart_config = {
            .port_num = modem->config.uart_port,
            .data_bits = UART_DATA_8_BITS, .stop_bits = UART_STOP_BITS_1,
            .parity = UART_PARITY_DISABLE, .flow_control = ESP_MODEM_FLOW_CONTROL_NONE,
            .source_clk = ESP_MODEM_DEFAULT_UART_CLK,
            .baud_rate = (int)modem->config.baud_rate,
            .tx_io_num = modem->config.tx_gpio, .rx_io_num = modem->config.rx_gpio,
            .rts_io_num = -1, .cts_io_num = -1,
            .rx_buffer_size = 8192u, .tx_buffer_size = 4096u, .event_queue_size = 30u,
        },
    };
    const esp_modem_dce_config_t dce = ESP_MODEM_DCE_DEFAULT_CONFIG(modem->config.apn.apn);
    modem->dce = esp_modem_new(&dte, &dce, modem->netif);
    if (modem->dce == NULL) return H2_PAL_ERR_NO_MEMORY;
    return H2_PAL_OK;
}

static h2_pal_result_t transport_status(void *user, h2_pal_modem_data_status_t *out_status) {
    h2_esp_lierda_modem_t *modem = user;
    memset(out_status, 0, sizeof(*out_status));
    h2_pal_result_t rc = lock(modem);
    if (rc != H2_PAL_OK) return rc;
    *out_status = modem->data;
    return unlock(modem, H2_PAL_OK);
}

static h2_pal_result_t transport_command(
    void *user, const char *command, char *response, size_t size, uint32_t timeout_ms) {
    h2_esp_lierda_modem_t *modem = user;
    if (size < CONFIG_ESP_MODEM_C_API_STR_MAX || timeout_ms > INT_MAX || modem->dce == NULL) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    h2_pal_modem_data_status_t status;
    h2_pal_result_t rc = transport_status(modem, &status);
    if (rc != H2_PAL_OK) return rc;
    if (status.state != H2_PAL_MODEM_DATA_CLOSED) return H2_PAL_ERR_BUSY;
    response[0] = '\0';
    rc = map_error(esp_modem_at(modem->dce, command, response, (int)timeout_ms));
    /* 1.4.3 C API silently limits strings; conservatively reject the boundary,
     * rather than treating a truncated prefix as a complete reply. */
    if (rc == H2_PAL_OK && strlen(response) >= CONFIG_ESP_MODEM_C_API_STR_MAX - 1u) {
        return H2_PAL_ERR_TRUNCATED;
    }
    return rc;
}

static h2_pal_result_t transport_data_open(
    void *user, const h2_pal_modem_apn_config_t *apn, uint32_t timeout_ms) {
    h2_esp_lierda_modem_t *modem = user;
    if (modem->dce == NULL) return H2_PAL_ERR_CLOSED;
    /* GenericModule setup_data_mode uses its own stored PDP context. Update
     * that context as well as the provider's CGDCONT before every new dial. */
    esp_err_t error = esp_modem_set_apn(modem->dce, apn->apn);
    if (error != ESP_OK) return data_failure(modem, map_error(error));
    esp_netif_auth_type_t auth = NETIF_PPP_AUTHTYPE_NONE;
    if (apn->username[0] != '\0' || apn->password[0] != '\0') {
#if CONFIG_LWIP_PPP_PAP_SUPPORT
        auth = (esp_netif_auth_type_t)(auth | NETIF_PPP_AUTHTYPE_PAP);
#endif
#if CONFIG_LWIP_PPP_CHAP_SUPPORT
        auth = (esp_netif_auth_type_t)(auth | NETIF_PPP_AUTHTYPE_CHAP);
#endif
        if (auth == NETIF_PPP_AUTHTYPE_NONE) return data_failure(modem, H2_PAL_ERR_UNSUPPORTED);
    }
    error = esp_netif_ppp_set_auth(modem->netif, auth, apn->username, apn->password);
    if (error != ESP_OK) return data_failure(modem, map_error(error));
    h2_pal_result_t rc = lock(modem);
    if (rc != H2_PAL_OK) return rc;
    modem->data_mode = true; /* fence AT before the potentially ambiguous dial */
    modem->command_confirmed = false;
    modem->closing = false;
    modem->data = (h2_pal_modem_data_status_t){.state = H2_PAL_MODEM_DATA_OPENING};
    xEventGroupClearBits(modem->events, LIERDA_IP | LIERDA_STOP | LIERDA_ERROR);
    rc = unlock(modem, H2_PAL_OK);
    if (rc != H2_PAL_OK) return rc;
    error = esp_modem_set_mode(modem->dce, ESP_MODEM_MODE_DATA);
    if (error != ESP_OK) return data_failure(modem, map_error(error));
    const EventBits_t bits = xEventGroupWaitBits(modem->events, LIERDA_IP | LIERDA_ERROR,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms != 0u ? timeout_ms : 45000u));
    if ((bits & LIERDA_ERROR) != 0u) return data_failure(modem, H2_PAL_ERR_IO);
    if ((bits & LIERDA_IP) == 0u) return data_failure(modem, H2_PAL_ERR_TIMEOUT);
    h2_pal_modem_data_status_t status;
    rc = transport_status(modem, &status);
    if (rc != H2_PAL_OK) return rc;
    if (status.state != H2_PAL_MODEM_DATA_OPEN || !status.ip4_valid) return data_failure(modem, H2_PAL_ERR_IO);
    rc = h2_esp_platform_netif_reconcile_default();
    return rc != H2_PAL_OK ? data_failure(modem, rc) : rc;
}

static h2_pal_result_t transport_data_close(void *user, uint32_t timeout_ms) {
    h2_esp_lierda_modem_t *modem = user;
    if (modem->dce == NULL) return H2_PAL_OK;
    h2_pal_result_t rc = lock(modem);
    if (rc != H2_PAL_OK) return rc;
    const bool need_stop = modem->data_mode;
    const bool need_command = need_stop && !modem->command_confirmed;
    modem->closing = true;
    modem->data = (h2_pal_modem_data_status_t){.state = H2_PAL_MODEM_DATA_CLOSING};
    if (need_command) xEventGroupClearBits(modem->events, LIERDA_STOP);
    rc = unlock(modem, H2_PAL_OK);
    if (rc != H2_PAL_OK) return rc;
    if (need_command) {
        const esp_err_t error = esp_modem_set_mode(modem->dce, ESP_MODEM_MODE_COMMAND);
        if (error != ESP_OK) return data_failure(modem, map_error(error));
        rc = lock(modem);
        if (rc != H2_PAL_OK) return rc;
        modem->command_confirmed = true;
        rc = unlock(modem, H2_PAL_OK);
        if (rc != H2_PAL_OK) return rc;
    }
    if (need_stop) {
        const EventBits_t bits = xEventGroupWaitBits(modem->events, LIERDA_STOP,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms != 0u ? timeout_ms : 5000u));
        if ((bits & LIERDA_STOP) == 0u) return data_failure(modem, H2_PAL_ERR_TIMEOUT);
    }
    /* PPP netif must not preserve an old address for the next session. */
    const esp_netif_ip_info_t empty_ip = {0};
    const esp_err_t error = esp_netif_set_ip_info(modem->netif, &empty_ip);
    if (error != ESP_OK) return data_failure(modem, map_error(error));
    rc = lock(modem);
    if (rc != H2_PAL_OK) return rc;
    modem->data_mode = false;
    modem->command_confirmed = true;
    modem->closing = false;
    memset(&modem->data, 0, sizeof(modem->data));
    rc = unlock(modem, H2_PAL_OK);
    return rc != H2_PAL_OK ? rc : h2_esp_platform_netif_reconcile_default();
}

h2_pal_result_t h2_esp_lierda_modem_create(
    const h2_esp_lierda_modem_config_t *config, h2_esp_lierda_modem_t **out_modem) {
    if (out_modem == NULL) return H2_PAL_ERR_INVALID_ARG;
    *out_modem = NULL;
    if (config == NULL || config->allocator == NULL || config->allocator->vtable == NULL ||
        config->allocator->vtable->alloc == NULL || config->allocator->vtable->free == NULL ||
        config->sync_api == NULL || config->sync_api->vtable == NULL ||
        config->sync_api->vtable->create_mutex == NULL || config->sync_api->vtable->destroy_mutex == NULL ||
        config->sync_api->vtable->lock_mutex == NULL || config->sync_api->vtable->unlock_mutex == NULL ||
        config->model != H2_LIERDA_MODEM_MODEL_NT26KCNB20NNC || config->power == NULL ||
        config->uart_port < 0 || config->uart_port >= UART_NUM_MAX ||
        config->tx_gpio < 0 || config->tx_gpio >= GPIO_NUM_MAX ||
        config->rx_gpio < 0 || config->rx_gpio >= GPIO_NUM_MAX || config->tx_gpio == config->rx_gpio ||
        config->baud_rate == 0u || config->baud_rate > INT_MAX ||
        config->dte_priority >= configMAX_PRIORITIES) return H2_PAL_ERR_INVALID_ARG;
#if !CONFIG_LWIP_PPP_SUPPORT || !CONFIG_LWIP_IPV4 || !CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT
    return H2_PAL_ERR_UNSUPPORTED;
#endif
    h2_esp_lierda_modem_t *modem = h2_pal_mem_alloc(config->allocator, sizeof(*modem));
    if (modem == NULL) return H2_PAL_ERR_NO_MEMORY;
    memset(modem, 0, sizeof(*modem));
    modem->config = *config;
    const h2_pal_mutex_config_t mutex_config = {
        .name = "lierda/ppp-state", .allocator = config->allocator,
    };
    h2_pal_result_t rc = h2_pal_mutex_create(config->sync_api, &mutex_config, &modem->mutex);
    if (rc == H2_PAL_OK && modem->mutex == NULL) rc = H2_PAL_ERR_INVALID_STATE;
    if (rc == H2_PAL_OK) {
        const h2_lierda_modem_config_t provider_config = {
            .model = config->model, .allocator = config->allocator,
            .sync_api = config->sync_api, .apn = config->apn,
            .transport = {.user = modem, .open = transport_open, .close = transport_close,
                .command = transport_command, .data_open = transport_data_open,
                .data_close = transport_data_close, .data_status = transport_status},
        };
        rc = h2_lierda_modem_create(&provider_config, &modem->provider);
    }
    if (rc != H2_PAL_OK) {
        if (modem->provider != NULL) {
            *out_modem = modem;
            return rc;
        }
        if (modem->mutex != NULL && h2_pal_mutex_destroy(config->sync_api, modem->mutex) != H2_PAL_OK) {
            *out_modem = modem;
            return rc;
        }
        h2_pal_mem_free(config->allocator, modem);
        return rc;
    }
    modem->initialized = true;
    *out_modem = modem;
    return H2_PAL_OK;
}

h2_pal_result_t h2_esp_lierda_modem_destroy(h2_esp_lierda_modem_t *modem) {
    if (modem == NULL) return H2_PAL_ERR_INVALID_ARG;
    if (modem->provider != NULL) {
        const h2_pal_result_t rc = h2_lierda_modem_destroy(modem->provider);
        if (rc != H2_PAL_OK) return rc;
        modem->provider = NULL;
    }
    if (modem->mutex != NULL) {
        const h2_pal_result_t rc = h2_pal_mutex_destroy(modem->config.sync_api, modem->mutex);
        if (rc != H2_PAL_OK) return rc;
    }
    const h2_pal_mem_api_t *allocator = modem->config.allocator;
    memset(modem, 0, sizeof(*modem));
    h2_pal_mem_free(allocator, modem);
    return H2_PAL_OK;
}

h2_pal_modem_api_t *h2_esp_lierda_modem_api(h2_esp_lierda_modem_t *modem) {
    return modem != NULL && modem->initialized ? h2_lierda_modem_api(modem->provider) : NULL;
}
