#include "h2_web_application_host.h"
#include "h2_web_main_thread.h"
#include <stdatomic.h>

#include "h2/pal/h2_pal_unsupported.h"
#include "h2_lvgl_platform.h"
#include "h2_web_fs.h"
#include "h2_yyjson_json.h"

#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct h2_web_application_host {
    const h2_web_application_host_config_t *config;
    h2_web_platform_t *platform;
    _Atomic(h2_runtime_t *) runtime;
    _Atomic(h2_application_host_t *) common;
    _Atomic int stop_requested;
    _Atomic int input_error;
    _Atomic int input_closed, input_backpressure;
    _Atomic unsigned input_readers;
    h2_web_application_host_entry_fn entry;
    void *user;
    h2_application_host_config_t app;
    h2_yyjson_json_t *json;
    const h2_pal_fs_api_t *fs;
    size_t allocation_bytes;
    h2_web_fs_t *owned_fs;
    int lvgl;
    int fs_result, destroy_result;
    h2_web_application_host_config_t owned_config;
    h2_web_application_host_button_t
        owned_buttons[H2_WEB_APPLICATION_HOST_MAX_BUTTONS];
    h2_web_application_host_hardware_t owned_hardware;
} h2_web_application_host_t;

static _Atomic(h2_web_application_host_t *) s_host;
/* Keep admission closed while UI publication is detached for destruction. */
static _Atomic int s_admission_closed;

/* Raw PAL Web targets can use the default canvas without a generated board.
 * h2_web_app's generated board definition overrides this weak default. */
__attribute__((weak)) const h2_web_application_board_t h2_web_application_board = {
    .display_width = 240, .display_height = 240};

/* Copy structural configuration and strings. Opaque callback contexts remain
 * borrowed: their owner must retain them when run returns RETAINED. */
static int host_string_bytes(size_t *bytes, const char *text) {
    if (text == NULL)
        return 1;
    size_t length = strlen(text);
    if (length == SIZE_MAX || *bytes > SIZE_MAX - length - 1u)
        return 0;
    *bytes += length + 1u;
    return 1;
}

static const char *host_copy_string(char **cursor, const char *text) {
    if (text == NULL)
        return NULL;
    size_t length = strlen(text) + 1u;
    char *copy = *cursor;
    memcpy(copy, text, length);
    *cursor += length;
    return copy;
}

static h2_web_application_host_t *
host_create(const h2_web_application_host_config_t *config,
            h2_web_application_host_entry_fn entry, void *user) {
    size_t bytes = sizeof(h2_web_application_host_t);
    if (config->readonly_root_count > (SIZE_MAX - bytes) / sizeof(char *))
        return NULL;
    bytes += config->readonly_root_count * sizeof(char *);
    if (!host_string_bytes(&bytes, config->name) ||
        !host_string_bytes(&bytes, config->persistent_root) ||
        !host_string_bytes(&bytes, config->firmware_version))
        return NULL;
    for (size_t i = 0; i < config->readonly_root_count; ++i)
        if (!host_string_bytes(&bytes, config->readonly_roots[i]))
            return NULL;
    for (size_t i = 0; i < config->button_count; ++i)
        if (!host_string_bytes(&bytes, config->buttons[i].key) ||
            !host_string_bytes(&bytes, config->buttons[i].name))
            return NULL;
    h2_web_application_host_t *host = calloc(1, bytes);
    if (host == NULL)
        return NULL;
    atomic_init(&host->runtime, NULL);
    atomic_init(&host->common, NULL);
    atomic_init(&host->stop_requested, 0);
    atomic_init(&host->input_error, H2_PAL_OK);
    atomic_init(&host->input_closed, 0);
    atomic_init(&host->input_backpressure, 0);
    atomic_init(&host->input_readers, 0u);
    host->allocation_bytes = bytes;
    host->owned_config = *config;
    host->config = &host->owned_config;
    host->entry = entry;
    host->user = user;
    const char **roots = (const char **)(host + 1);
    char *cursor = (char *)(roots + config->readonly_root_count);
    host->owned_config.name = host_copy_string(&cursor, config->name);
    host->owned_config.persistent_root =
        host_copy_string(&cursor, config->persistent_root);
    host->owned_config.firmware_version =
        host_copy_string(&cursor, config->firmware_version);
    host->owned_config.readonly_roots = roots;
    for (size_t i = 0; i < config->readonly_root_count; ++i)
        roots[i] = host_copy_string(&cursor, config->readonly_roots[i]);
    host->owned_config.buttons = host->owned_buttons;
    for (size_t i = 0; i < config->button_count; ++i) {
        host->owned_buttons[i] = config->buttons[i];
        host->owned_buttons[i].key =
            host_copy_string(&cursor, config->buttons[i].key);
        host->owned_buttons[i].name =
            host_copy_string(&cursor, config->buttons[i].name);
    }
    if (config->hardware != NULL) {
        host->owned_hardware = *config->hardware;
        host->owned_config.hardware = &host->owned_hardware;
    }
    return host;
}

static const h2_pal_periph_single_button_payload_t s_button_payload = {
    .delivery = H2_PAL_BUTTON_DELIVERY_PUSH_EDGE,
};

// Peripheral ids are button index + 1.
/* The config key overrides; otherwise the web board's key for that name. */
static const char *h2_web_application_host_button_key(
    const h2_web_application_host_button_t *button) {
    if (button->key != NULL)
        return button->key;
    for (size_t index = 0u;
         button->name != NULL && index < h2_web_application_board.button_count;
         ++index) {
        const h2_web_application_board_button_t *board =
            &h2_web_application_board.buttons[index];
        if (strcmp(board->name, button->name) == 0)
            return board->key[0] != '\0' ? board->key : NULL;
    }
    return NULL;
}

static h2_pal_periph_info_t h2_web_application_host_button_info(size_t index) {
    h2_pal_periph_info_t info = {
        .id = (h2_pal_periph_id_t)(index + 1u),
        .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
        .payload = &s_button_payload,
        .payload_size = sizeof(s_button_payload),
    };
    /* Peripheral name: the board Button name, else its resolved key; a NULL
     * config key never reaches the format. */
    const h2_web_application_host_button_t *button =
        &s_host->config->buttons[index];
    const char *key = h2_web_application_host_button_key(button);
    (void)snprintf(info.name, sizeof(info.name), "%s",
                   button->name != NULL ? button->name
                   : key != NULL        ? key
                                        : "button");
    return info;
}

static h2_pal_result_t h2_web_application_host_list_components(
    void *user, h2_runtime_component_t filter,
    h2_runtime_component_mapping_cb_t callback, void *callback_user) {
    (void)user;
    if (callback == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (filter != H2_RUNTIME_COMPONENT_BUTTON || s_host == NULL)
        return H2_PAL_OK;
    for (size_t index = 0u; index < s_host->config->button_count; ++index) {
        const h2_runtime_component_mapping_entry_t entry = {
            s_host->config->buttons[index].component_id,
            (h2_pal_periph_id_t)(index + 1u)};
        const h2_pal_result_t result = callback(callback_user, &entry);
        if (result != H2_PAL_OK)
            return result;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_application_host_component_periph(void *user,
                                         h2_runtime_component_id_t component_id,
                                         h2_pal_periph_id_t *out_periph_id) {
    (void)user;
    if (out_periph_id == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    for (size_t index = 0u;
         s_host != NULL && index < s_host->config->button_count; ++index) {
        if (s_host->config->buttons[index].component_id == component_id) {
            *out_periph_id = (h2_pal_periph_id_t)(index + 1u);
            return H2_PAL_OK;
        }
    }
    return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t
h2_web_application_host_list_periph(void *user, h2_pal_periph_type_t filter,
                                    h2_pal_periph_cb_t callback,
                                    void *callback_user) {
    (void)user;
    if (callback == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (s_host == NULL || (filter != H2_PAL_PERIPH_TYPE_ANY &&
                           filter != H2_PAL_PERIPH_TYPE_SINGLE_BUTTON))
        return H2_PAL_OK;
    for (size_t index = 0u; index < s_host->config->button_count; ++index) {
        const h2_pal_periph_info_t info =
            h2_web_application_host_button_info(index);
        const h2_pal_result_t result = callback(callback_user, &info);
        if (result != H2_PAL_OK)
            return result;
    }
    return H2_PAL_OK;
}

static h2_pal_result_t
h2_web_application_host_get_periph(void *user, h2_pal_periph_id_t id,
                                   h2_pal_periph_info_t *out_info) {
    (void)user;
    if (out_info == NULL)
        return H2_PAL_ERR_INVALID_ARG;
    if (s_host == NULL || id == 0u || id > s_host->config->button_count)
        return H2_PAL_ERR_NOT_FOUND;
    *out_info = h2_web_application_host_button_info((size_t)id - 1u);
    return H2_PAL_OK;
}

static const h2_runtime_component_mapper_vtable_t s_mapper_vtable = {
    .list = h2_web_application_host_list_components,
    .get_periph_id = h2_web_application_host_component_periph,
};
static const h2_runtime_component_mapper_t s_mapper = {
    .vtable = &s_mapper_vtable,
};
static const h2_pal_periph_vtable_t s_periph_vtable = {
    .list = h2_web_application_host_list_periph,
    .get = h2_web_application_host_get_periph,
};
static const h2_pal_periph_api_t s_periph = {
    .vtable = &s_periph_vtable,
};

/* Called by the shell with the button index and 1 down / 0 up. */
EMSCRIPTEN_KEEPALIVE int h2_web_application_host_button(int index,
                                                        int pressed) {
    h2_web_application_host_t *host = atomic_load(&s_host);
    // Retention permanently closes admission; BUSY is reserved for retries.
    if (host != NULL && (atomic_load(&host->input_closed) ||
        h2_application_host_is_retained(atomic_load(&host->common))))
        return H2_PAL_ERR_CLOSED;
    h2_runtime_t *runtime = host != NULL ? atomic_load(&host->runtime) : NULL;
    if (host == NULL || runtime == NULL || index < 0 ||
        (size_t)index >= host->config->button_count)
        return H2_PAL_ERR_UNAVAILABLE;
    const h2_web_application_host_hardware_t *hardware =
        host->config->hardware;
    h2_pal_result_t rc;
    if (hardware != NULL && hardware->button != NULL)
        rc = hardware->button(hardware->user, runtime,
                                host->config->buttons[index].component_id,
                                pressed);
    else
        // Edges use peripheral ids; the mapper resolves the component.
        rc = h2_runtime_button_push_edge(
            runtime, (h2_pal_periph_id_t)(index + 1),
            pressed ? H2_RUNTIME_BUTTON_EDGE_DOWN : H2_RUNTIME_BUTTON_EDGE_UP);
    if (rc == H2_PAL_ERR_TIMEOUT || rc == H2_PAL_ERR_BUSY ||
        rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_FULL)
        // Health takes a PAL mutex and must run on the pump Worker, not UI.
        atomic_store(&host->input_backpressure, 1);
    return rc;
}

/* The shell's JavaScript owns keyboard and layout input for each Button. */

/* clang-format off */
EM_JS(void, h2_web_application_host_bind_button,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion,
    ["i32", "pointer", "pointer", "i32", "i32", "i32", "i32", "i32"], "i32",
    (index, key, name, ioError, timeoutError, busyError, wouldBlockError, fullError) => {
      try {
        const bind = globalThis.h2WebApplicationHostBindButton;
        if (!globalThis.document) return 0;
        if (typeof bind !== 'function') return ioError;
        bind(index, key ? UTF8ToString(key) : "", name ? UTF8ToString(name) : "",
             ioError, timeoutError, busyError, wouldBlockError, fullError);
        return 0;
      } catch (_) { return ioError; }
    });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_application_host_unbind_buttons,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["u32", "i32"], "i32",
    (count, ioError) => {
      try {
        const unbind = globalThis.h2WebApplicationHostUnbindButtons;
        if (globalThis.document && count && typeof unbind !== 'function') return ioError;
        return Promise.resolve(unbind?.()).then(() => 0, () => ioError);
      } catch (_) { return ioError; }
    });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_application_host_status,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["pointer"], null,
    (text) => {
      try {
        const status = globalThis.document && document.getElementById('status');
        if (status) status.textContent = UTF8ToString(text);
      } catch (_) { /* Console markers remain available when UI status is absent. */ }
    });
});
/* clang-format on */

/* clang-format off */
EM_JS(void, h2_web_application_host_size_canvas,
      (void *context, h2_web_main_result_t *result, h2_web_main_completion_t *completion), {
  h2WebMain(context, result, completion, ["i32", "i32", "i32"], "i32",
    (width, height, ioError) => {
      try {
        const canvas = Module['canvas'];
        if (canvas) { canvas.width = width; canvas.height = height; }
        return 0;
      } catch (_) { return ioError; }
    });
});
/* clang-format on */

/* Shells call this from a Stop button; endless Apps end cooperatively. */
EMSCRIPTEN_KEEPALIVE void h2_web_application_host_request_stop(void) {
    h2_web_application_host_t *host = atomic_load(&s_host);
    if (host != NULL) {
        atomic_store(&host->stop_requested, 1);
        h2_application_host_request_stop(atomic_load(&host->common));
    }
}

/* Called only after the UI outbox has closed admission and canceled retries. */
EMSCRIPTEN_KEEPALIVE void h2_web_application_host_input_failed(int reason) {
    h2_web_application_host_t *host = atomic_load(&s_host);
    if (host == NULL)
        return;
    atomic_store(&host->input_closed, 1);
    int expected = H2_PAL_OK;
    (void)atomic_compare_exchange_strong(
        &host->input_error, &expected, reason < 0 ? reason : H2_PAL_ERR_IO);
    h2_web_application_host_request_stop();
}

static void h2_web_application_host_mark(const h2_web_application_host_t *host,
                                         const char *stage) {
    char line[160];
    (void)snprintf(line, sizeof(line), "H2_WEB_APP name=%s stage=%s",
                   host->config->name, stage);
    puts(line);
    (void)h2_web_main_call(h2_web_application_host_status,
                           (const void *[]){&(const char *){line}});
}

h2_web_platform_t *
h2_web_application_host_platform(h2_application_host_t *host) {
    const h2_pal_application_host_api_t *api =
        h2_application_host_provider(host);
    h2_web_application_host_t *web = api != NULL ? api->user : NULL;
    return web != NULL ? web->platform : NULL;
}

const h2_pal_json_api_t *h2_web_application_host_json(h2_application_host_t *host) {
    const h2_pal_application_host_api_t *api = h2_application_host_provider(host);
    h2_web_application_host_t *web = api != NULL ? api->user : NULL;
    return web != NULL ? h2_yyjson_json_api(web->json) : NULL;
}

static h2_runtime_config_t
h2_web_application_host_runtime_config(h2_web_application_host_t *host,
                                       const h2_pal_fs_api_t *fs) {
    h2_web_platform_t *platform = host->platform;
    h2_runtime_config_t config = {0};
    config.board = "browser";
    config.target = "webassembly";
    config.chip = "wasm32";
    config.firmware_info = host->config->firmware_version != NULL
        ? h2_web_platform_firmware_info_api(platform)
        : h2_pal_unsupported_firmware_info_api();
    config.mem = h2_web_platform_mem_api();
    config.log = h2_web_platform_log_api();
    config.time = h2_web_platform_time_api(platform);
    config.timer = h2_web_platform_timer_api(platform);
    config.task = h2_web_platform_task_api(platform);
    config.queue = h2_web_platform_queue_api(platform);
    config.sync = h2_web_platform_sync_api(platform);
    config.fs = fs != NULL ? fs : h2_pal_unsupported_fs_api();
    config.disk = h2_pal_unsupported_disk_api();
    config.pref = h2_web_platform_pref_api(platform);
    config.crypto = h2_web_platform_crypto_api(platform);
    config.http = h2_web_platform_http_api(platform);
    config.net = h2_pal_unsupported_net_api();
    config.netif = h2_web_platform_netif_api(platform);
    config.mqtt = h2_pal_unsupported_mqtt_api();
    config.webrtc = h2_web_platform_webrtc_api(platform);
    config.wifi_sta = h2_pal_unsupported_wifi_sta_api();
    config.wifi_ap = h2_pal_unsupported_wifi_ap_api();
    config.wifi_csi = h2_pal_unsupported_wifi_csi_api();
    config.wifi_settings = h2_pal_unsupported_wifi_settings_api();
    config.ble_host = h2_pal_unsupported_ble_host_api();
    config.modem = h2_pal_unsupported_modem_api();
    if (h2_web_platform_fake_wifi_sta_api(platform) != NULL) {
        config.wifi_sta = h2_web_platform_fake_wifi_sta_api(platform);
        config.wifi_settings = h2_web_platform_fake_wifi_settings_api(platform);
        config.modem = h2_web_platform_fake_modem_api(platform);
    }
    config.power = h2_pal_unsupported_power_api();
    config.display = h2_web_platform_display_api(platform);
    config.audio = h2_web_platform_audio_api(platform);
    config.audio_decoder = h2_web_platform_audio_decoder_api(platform);
    config.periph = host->config->button_count != 0u
                        ? &s_periph
                        : h2_pal_unsupported_periph_api();
    config.button = h2_pal_unsupported_button_api();
    config.touch = h2_web_platform_touch_api(platform);
    config.buzzer = h2_pal_unsupported_buzzer_api();
    config.nfc = h2_pal_unsupported_nfc_api();
    config.nfc_card_emulation = h2_pal_unsupported_nfc_card_emulation_api();
    config.imu = h2_pal_unsupported_imu_api();
    config.gpio_irq = h2_pal_unsupported_gpio_irq_api();
    config.led = h2_pal_unsupported_led_api();
    config.switch_api = h2_pal_unsupported_switch_api();
    config.pwm_switch = h2_pal_unsupported_pwm_switch_api();
    config.input = h2_pal_unsupported_input_api();
    config.system_event = h2_web_platform_system_event_api(platform);
    config.video_decoder = h2_web_platform_video_decoder_api(platform);
    config.component_mapper =
        host->config->button_count != 0u ? &s_mapper : NULL;
    config.event_queue_capacity = H2_RUNTIME_DEFAULT_EVENT_QUEUE_CAPACITY;
    return config;
}

/* Board hardware may replace any provider, including the Button mapping. */
static h2_pal_result_t
h2_web_application_host_configure_runtime(const h2_web_application_host_t *host,
                                          h2_runtime_config_t *config) {
    const h2_web_application_host_hardware_t *hardware = host->config->hardware;
    if (hardware != NULL && hardware->configure_runtime != NULL) {
        h2_pal_result_t rc =
            hardware->configure_runtime(hardware->user, config);
        if (rc != H2_PAL_OK)
            return rc;
    }
    if (host->app.configure != NULL) {
        h2_pal_result_t rc = host->app.configure(host->app.user, config);
        if (rc != H2_PAL_OK) return rc;
    }
    if (config->led == h2_pal_unsupported_led_api()) {
        h2_pal_result_t rc =
            h2_web_platform_configure_leds(host->platform, config->periph);
        if (rc != H2_PAL_OK)
            return rc;
        config->led = h2_web_platform_led_api(host->platform);
    }
    return H2_PAL_OK;
}

static h2_pal_result_t web_prepare(void *user, h2_runtime_config_t *out) {
    h2_web_application_host_t *host = user;
    const h2_web_application_host_config_t *config = host->config;
    const int32_t width = config->display_width
                              ? config->display_width
                              : h2_web_application_board.display_width;
    const int32_t height = config->display_height
                               ? config->display_height
                               : h2_web_application_board.display_height;
    if (width <= 0 || height <= 0)
        return H2_PAL_ERR_INVALID_ARG;
    h2_web_main_result_t sized = h2_web_main_call(
        h2_web_application_host_size_canvas,
        (const void *[]){&(int){width}, &(int){height}, &(int){H2_PAL_ERR_IO}});
    if (sized.i32 != H2_PAL_OK)
        return sized.i32;
    const h2_web_platform_config_t pc = {.display_width = width,
                                         .display_height = height};
    host->platform = config->task_allocator != NULL
        ? h2_web_platform_create_with_task_allocator(&pc, config->task_allocator)
        : h2_web_platform_create(&pc);
    if (host->platform == NULL)
        return H2_PAL_ERR_NO_MEMORY;
    h2_pal_result_t rc = H2_PAL_OK;
    if (config->firmware_version != NULL)
        rc = h2_web_platform_configure_firmware_info(host->platform, config->firmware_version);
    if (rc == H2_PAL_OK && config->json)
        rc = h2_yyjson_json_create(h2_web_platform_mem_api(), &host->json);
    const h2_web_application_host_hardware_t *hw = config->hardware;
    if (rc == H2_PAL_OK && hw != NULL && hw->prepare != NULL)
        rc = hw->prepare(hw->user, host->platform);
    if (rc == H2_PAL_OK && config->persistent_root != NULL) {
        const h2_web_fs_config_t fc = {
            .persistent_root = config->persistent_root,
            .readonly_roots = config->readonly_roots,
            .readonly_root_count = config->readonly_root_count};
        rc = h2_web_fs_open(host->platform, &fc, &host->owned_fs);
        host->fs = h2_web_fs_api(host->owned_fs);
    }
    if (rc == H2_PAL_OK && config->lvgl) {
        const h2_lvgl_platform_config_t lc = {
            .allocator = h2_web_platform_mem_api(),
            .task_api = h2_web_platform_task_api(host->platform),
            .sync_api = h2_web_platform_sync_api(host->platform),
            .queue_api = h2_web_platform_queue_api(host->platform),
            .time_api = h2_web_platform_time_api(host->platform)};
        rc = h2_lvgl_platform_init(&lc) == 0 ? H2_PAL_OK
                                             : H2_PAL_ERR_UNAVAILABLE;
        host->lvgl = rc == H2_PAL_OK;
    }
    if (rc == H2_PAL_OK)
        *out = h2_web_application_host_runtime_config(host, host->fs);
    return rc;
}

static h2_pal_result_t web_configure(void *user, h2_runtime_config_t *config) {
    return h2_web_application_host_configure_runtime(user, config);
}

static h2_pal_result_t web_validate(void *user, h2_runtime_t *runtime) {
    h2_web_application_host_t *host = user;
    const h2_web_application_host_hardware_t *hw = host->config->hardware;
    h2_pal_result_t rc = hw != NULL && hw->validate_runtime != NULL
               ? hw->validate_runtime(hw->user, runtime)
               : H2_PAL_OK;
    return rc == H2_PAL_OK && host->app.validate != NULL
        ? host->app.validate(host->app.user, runtime) : rc;
}

static h2_pal_result_t web_release_runtime(void *user) {
    h2_web_application_host_t *host = user;
    const h2_web_application_host_hardware_t *hw = host->config->hardware;
    if (host->app.release_runtime != NULL) {
        h2_pal_result_t rc = host->app.release_runtime(host->app.user);
        if (rc != H2_PAL_OK) return rc;
    }
    return hw != NULL && hw->release_runtime != NULL
               ? hw->release_runtime(hw->user)
               : H2_PAL_OK;
}

static h2_pal_result_t web_activate(void *user, h2_runtime_t *runtime) {
    h2_web_application_host_t *host = user;
    host->runtime = runtime;
    // Start the consumer before publishing any DOM producer. Partial bind
    // rollback can then drain its outbox while Runtime input is still live.
    h2_pal_result_t rc = host->config->button_count != 0
        ? h2_runtime_input_start(runtime, NULL) : H2_PAL_OK;
    for (size_t i = 0; rc == H2_PAL_OK && i < host->config->button_count; ++i) {
        h2_web_main_result_t bound = h2_web_main_call(
            h2_web_application_host_bind_button,
            (const void *[]){&(int){(int)i},
                             &(const char *){h2_web_application_host_button_key(
                                 &host->config->buttons[i])},
                             &(const char *){host->config->buttons[i].name},
                             &(int){H2_PAL_ERR_IO},
                             &(int){H2_PAL_ERR_TIMEOUT},
                             &(int){H2_PAL_ERR_BUSY},
                             &(int){H2_PAL_ERR_WOULD_BLOCK},
                             &(int){H2_PAL_ERR_FULL}});
        rc = bound.i32;
    }
    return rc;
}

static h2_pal_result_t web_deactivate(void *user) {
    h2_web_application_host_t *host = user;
    h2_web_main_result_t unbound = h2_web_main_call(
        h2_web_application_host_unbind_buttons,
        (const void *[]){&(uint32_t){(uint32_t)host->config->button_count},
                         &(int){H2_PAL_ERR_IO}});
    if (unbound.i32 == H2_PAL_OK) {
        host->runtime = NULL;
        // The driver may have borrowed Runtime for a Worker-side health read.
        // Clear admission first, then wait before common destroys Runtime.
        while (atomic_load(&host->input_readers) != 0u) {
            h2_pal_result_t rc = h2_pal_time_sleep_ms(
                h2_web_platform_time_api(host->platform), 1u);
            if (rc != H2_PAL_OK)
                return rc;
        }
    }
    return unbound.i32;
}

static h2_pal_result_t web_poll(void *user, int *out_stop) {
    h2_web_application_host_t *host = user;
    *out_stop = 0;
    h2_pal_result_t rc = h2_web_platform_pump(host->platform, 64u, NULL);
    if (rc != H2_PAL_OK || !atomic_exchange(&host->input_backpressure, 0))
        return rc;
    atomic_fetch_add(&host->input_readers, 1u);
    h2_runtime_t *runtime = atomic_load(&host->runtime);
    if (runtime != NULL) {
        h2_runtime_input_status_t status;
        rc = h2_runtime_input_status(runtime, &status);
        h2_pal_result_t fault = rc != H2_PAL_OK ? rc : status.worker_result;
        if (fault != H2_PAL_OK) {
            // A failed consumer cannot drain its otherwise-live raw queue.
            // Close UI retries, latch the cause and cooperatively stop App.
            int expected = H2_PAL_OK;
            atomic_store(&host->input_closed, 1);
            (void)atomic_compare_exchange_strong(&host->input_error, &expected,
                fault < 0 ? fault : H2_PAL_ERR_IO);
            *out_stop = 1;
        }
    }
    atomic_fetch_sub(&host->input_readers, 1u);
    return rc;
}

static h2_pal_result_t web_release(void *user) {
    h2_web_application_host_t *host = user;
    const h2_web_application_host_hardware_t *hw = host->config->hardware;
    h2_pal_result_t json_rc = h2_yyjson_json_destroy(&host->json);
    if (json_rc != H2_PAL_OK) return json_rc;
    if (host->platform != NULL && hw != NULL && hw->release_prepare != NULL) {
        h2_pal_result_t rc = hw->release_prepare(hw->user);
        if (rc != H2_PAL_OK)
            return rc;
    }
    if (host->lvgl) {
        h2_lvgl_platform_deinit();
        host->lvgl = 0;
    }
    host->fs_result = h2_web_fs_close(host->owned_fs);
    if (host->fs_result == H2_PAL_ERR_BUSY ||
        host->fs_result == H2_PAL_ERR_TIMEOUT)
        return host->fs_result;
    host->owned_fs = NULL;
    host->fs = NULL;
    host->destroy_result = h2_web_platform_destroy(host->platform);
    if (host->destroy_result == H2_PAL_OK)
        host->platform = NULL;
    return host->destroy_result != H2_PAL_OK ? host->destroy_result
                                             : host->fs_result;
}

static void web_notify(void *user, h2_pal_application_host_event_t event) {
    const char *stage = event == H2_PAL_APPLICATION_HOST_READY ? "ready"
                        : event == H2_PAL_APPLICATION_HOST_RUNNING
                            ? "running"
                            : "stop-requested";
    h2_web_application_host_mark(user, stage);
}

static const h2_pal_application_host_vtable_t web_vtable = {
    .prepare = web_prepare,
    .activate = web_activate,
    .poll = web_poll,
    .deactivate = web_deactivate,
    .release = web_release,
    .notify = web_notify};

static h2_pal_result_t web_entry(h2_application_host_t *common,
                                 h2_runtime_t *runtime, void *user) {
    h2_web_application_host_t *host = user;
    return host->entry(common, runtime, host->user);
}

static void web_detach_ui(void *context, h2_web_main_result_t *result,
                          h2_web_main_completion_t *completion) {
    (void)context;
    (void)result;
    // Drain synchronous UI callbacks before freeing the Host they can borrow.
    atomic_store(&s_host, NULL);
    h2_web_main_complete(completion);
}

int h2_web_application_host_run(const h2_web_application_host_config_t *config,
                                h2_web_application_host_entry_fn entry,
                                void *user) {
    const h2_application_host_config_t app = {.entry = entry, .user = user};
    return h2_web_application_host_run_app(config, &app, NULL);
}

static h2_pal_result_t web_before(void *user) {
    h2_web_application_host_t *host = user;
    return host->app.before_prepare != NULL
        ? host->app.before_prepare(host->app.user) : H2_PAL_OK;
}

int h2_web_application_host_run_app(const h2_web_application_host_config_t *config,
                                  const h2_application_host_config_t *app,
                                  h2_application_host_result_t *out_result) {
    if (out_result != NULL) *out_result = (h2_application_host_result_t){0};
    h2_web_application_host_entry_fn entry = app != NULL ? app->entry : NULL;
    void *user = app != NULL ? app->user : NULL;
    if (config == NULL || config->name == NULL || entry == NULL ||
        app->provider != NULL ||
        config->button_count > H2_WEB_APPLICATION_HOST_MAX_BUTTONS ||
        (config->button_count && config->buttons == NULL) ||
        (config->readonly_root_count && config->readonly_roots == NULL))
        return 1;
    h2_web_application_host_t *host = host_create(config, entry, user);
    if (host == NULL)
        return 1;
    host->app = *app;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_admission_closed, &expected, 1)) {
        free(host);
        if (out_result != NULL) {
            out_result->application = H2_PAL_ERR_BUSY;
            out_result->cleanup = H2_PAL_ERR_BUSY;
            out_result->retained = 1;
        }
        return H2_WEB_APPLICATION_HOST_RETAINED;
    }
    atomic_store(&s_host, host);
    const h2_pal_application_host_api_t provider = {.user = host,
                                                    .vtable = &web_vtable};
    const h2_application_host_config_t hc = {
        .allocator = app->allocator != NULL ? app->allocator : h2_web_platform_mem_api(),
        .provider = &provider,
        .entry = web_entry,
        .user = host,
        .configure = web_configure,
        .release_runtime = web_release_runtime,
        .validate = web_validate,
        .run_ms = app->run_ms != 0u ? app->run_ms : config->run_ms,
        .stack_size = app->stack_size != 0u ? app->stack_size : config->stack_size,
        .control_stack_size = app->control_stack_size,
        .before_prepare = web_before};
    h2_application_host_result_t result = {0};
    h2_application_host_t *common = NULL;
    h2_pal_result_t rc = h2_application_host_create(&hc, &common);
    atomic_store(&host->common, common);
    if (rc == H2_PAL_OK) {
        if (atomic_load(&host->stop_requested))
            h2_application_host_request_stop(common);
        rc = h2_application_host_run(common, &result);
    } else result.application = rc;
    if (rc == H2_PAL_OK)
        rc = atomic_load(&host->input_error);
    if (result.application == H2_PAL_OK && result.cleanup == H2_PAL_OK &&
        rc != H2_PAL_OK)
        result.application = rc;
    if (!result.retained) {
        /* Detach on UI to drain callbacks that already borrowed common. The
         * separate admission latch prevents a new run entering this gap. */
        (void)h2_web_main_call(web_detach_ui, NULL);
        h2_pal_result_t destroyed = h2_application_host_destroy(&common);
        if (destroyed != H2_PAL_OK) {
            (void)h2_application_host_retain(common, host, destroyed);
            atomic_store(&s_host, host);
            result.cleanup = destroyed;
            result.retained = 1;
            rc = destroyed;
        } else {
            atomic_store(&host->common, NULL);
        }
    }
    if (out_result != NULL) *out_result = result;
    printf("H2_WEB_APP name=%s owner_bytes=%zu host=pal-application\n",
           host->config->name, host->allocation_bytes);
    char line[224];
    if (result.retained) {
        (void)snprintf(
            line, sizeof(line),
            "H2_WEB_APP name=%s result=FAIL rc=%d retained=1 owner_bytes=%zu",
            host->config->name, rc, host->allocation_bytes);
    } else {
        (void)snprintf(line, sizeof(line),
                       "H2_WEB_APP name=%s result=%s rc=%d fs=%d destroy=%d",
                       host->config->name, rc == H2_PAL_OK ? "PASS" : "FAIL",
                       rc, host->fs_result, host->destroy_result);
    }
    puts(line);
    (void)h2_web_main_call(h2_web_application_host_status,
                           (const void *[]){&(const char *){line}});
    if (result.retained)
        return H2_WEB_APPLICATION_HOST_RETAINED;
    free(host);
    atomic_store(&s_admission_closed, 0);
    return rc == H2_PAL_OK ? 0 : 1;
}
