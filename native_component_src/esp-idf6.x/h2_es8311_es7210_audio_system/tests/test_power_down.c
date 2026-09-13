#include "h2_esp_es8311_es7210_audio_system.h"
#include "fake_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Compile the complete platform implementation against SDK substitutes. */
#include "../src/h2_esp_es8311_es7210_platform_audio.c"

static uint8_t registers[2][256];
static struct { unsigned dev; uint8_t reg, value; } writes[1024];
static size_t write_count;
static int fail_write = -1;
static int removed, disabled, pa_enabled, pa_fail;
static TickType_t ticks;
static h2_esp_es8311_es7210_audio_system_t *running;
static int join_workers = 1;
static unsigned device_index(i2c_master_dev_handle_t dev) {
    assert(dev == (void *)1 || dev == (void *)2);
    return (unsigned)(uintptr_t)dev - 1u;
}
int i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t size, int timeout) {
    (void)timeout;
    assert(size == 2 && write_count < 1024);
    unsigned index = device_index(dev);
    writes[write_count].dev = index;
    writes[write_count].reg = data[0];
    writes[write_count].value = data[1];
    if ((int)write_count++ == fail_write) return ESP_FAIL;
    registers[index][data[0]] = data[1];
    return ESP_OK;
}
int i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *data, size_t size, uint8_t *out, size_t len, int timeout) {
    (void)timeout;
    assert(size == 1 && len == 1);
    *out = registers[device_index(dev)][*data];
    return ESP_OK;
}
int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out) {
    (void)bus; *out = cfg->device_address == 0x18 ? (void *)1 : (void *)2; return ESP_OK;
}
int i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *out) { (void)cfg; *out = (void *)3; return ESP_OK; }
int i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *out) { (void)port; *out = (void *)3; return ESP_OK; }
int i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { (void)dev; assert(!pa_enabled); ++removed; return ESP_OK; }
int i2c_del_master_bus(i2c_master_bus_handle_t bus) { (void)bus; return ESP_OK; }
int i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) { (void)cfg; *tx = (void *)4; *rx = (void *)5; return ESP_OK; }
int i2s_channel_init_std_mode(i2s_chan_handle_t ch, const i2s_std_config_t *cfg) { (void)ch; (void)cfg; return ESP_OK; }
int i2s_channel_enable(i2s_chan_handle_t ch) { (void)ch; return ESP_OK; }
int i2s_channel_disable(i2s_chan_handle_t ch) { (void)ch; assert(registers[0][0x0d] == 0xfc && registers[1][6] == 7); ++disabled; return ESP_OK; }
int i2s_del_channel(i2s_chan_handle_t ch) { (void)ch; return ESP_OK; }
int i2s_channel_read(i2s_chan_handle_t ch, void *buf, size_t len, size_t *out, uint32_t timeout) { (void)ch; (void)buf; (void)len; (void)timeout; *out = 0; return ESP_ERR_TIMEOUT; }
int i2s_channel_write(i2s_chan_handle_t ch, const void *buf, size_t len, size_t *out, uint32_t timeout) { (void)ch; (void)buf; (void)timeout; *out = len; return ESP_OK; }
int gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
int gpio_set_level(gpio_num_t pin, unsigned level) { (void)pin; pa_enabled = (int)level; return ESP_OK; }
static int fake_pa(void *user, int enabled) { (void)user; if (pa_fail) return H2_AUDIO_ERR_IO; pa_enabled = enabled; return H2_AUDIO_OK; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)6; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) { (void)mutex; (void)timeout; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t mutex) { (void)mutex; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t delay) { ticks += delay; if (running && join_workers) { if (!running->mic_started) running->mic_task = NULL; if (!running->playback_started) running->playback_task = NULL; } }
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDeleteWithCaps(TaskHandle_t task) { (void)task; }
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *ctx, UBaseType_t priority, TaskHandle_t *out, BaseType_t core) { (void)fn; (void)name; (void)stack; (void)ctx; (void)priority; (void)core; *out = (void *)7; return pdPASS; }
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *ctx, UBaseType_t priority, TaskHandle_t *out, BaseType_t core, uint32_t caps) { (void)caps; return xTaskCreatePinnedToCore(fn, name, stack, ctx, priority, out, core); }
int h2_audio_mixer_init(h2_audio_mixer_t *m, const h2_audio_mixer_config_t *c) { (void)m; (void)c; return H2_AUDIO_OK; }
void h2_audio_mixer_deinit(h2_audio_mixer_t *m) { (void)m; }
int h2_audio_mixer_read(h2_audio_mixer_t *m, h2_audio_frame_t *f) { (void)m; f->bytes = 0; return H2_AUDIO_OK; }
int h2_audio_mixer_create_track(h2_audio_mixer_t *m, const h2_pal_audio_api_t *a, const h2_audio_track_config_t *c, h2_pal_audio_track_t **t) { (void)m; (void)a; (void)c; (void)t; return H2_AUDIO_ERR_UNSUPPORTED; }
int h2_esp_es8311_es7210_sr_init(h2_esp_es8311_es7210_sr_state_t *s, const h2_esp_es8311_es7210_audio_system_config_t *c) { (void)c; s->initialized = 1; return H2_AUDIO_OK; }
void h2_esp_es8311_es7210_sr_deinit(h2_esp_es8311_es7210_sr_state_t *s) { s->initialized = 0; }
void h2_esp_es8311_es7210_sr_reset(h2_esp_es8311_es7210_sr_state_t *s) { (void)s; }
int h2_esp_es8311_es7210_sr_process(h2_esp_es8311_es7210_sr_state_t *s, const h2_audio_frame_t *r, h2_audio_frame_t *o, uint32_t t) { (void)s; (void)r; (void)o; (void)t; return H2_AUDIO_ERR_UNSUPPORTED; }

static const h2_pal_queue_api_t queue_api = {0};
static const h2_pal_sync_api_t sync_api = {0};
static h2_esp_es8311_es7210_audio_system_config_t config(void) {
    return (h2_esp_es8311_es7210_audio_system_config_t){
        .sample_rate_hz = 16000, .frame_samples_per_channel = 160,
        .raw_channels = 4, .processed_channels = 1, .mic_channel_count = 1,
        .mic_channel_indices = {1}, .ref_channel_index = 0,
        .es7210_input_mask = 3, .es7210_ref_input_index = 0,
        .mclk_multiple = 384, .pa_gpio = -1, .set_pa = fake_pa,
        .es8311_i2c_addr = 0x18, .es7210_i2c_addr = 0x40,
        .codec_volume_default = 0xbf, .max_tracks = 1,
        .track_queue_frames = 2, .mic_queue_frames = 2,
        .mic_task_stack_size = 4096, .speaker_task_stack_size = 4096,
        .queue_api = &queue_api, .sync_api = &sync_api,
    };
}
static void open_system(h2_esp_es8311_es7210_audio_system_t *s) {
    h2_esp_es8311_es7210_audio_system_config_t cfg = config();
    assert(h2_esp_es8311_es7210_audio_system_init(s, &cfg) == H2_AUDIO_OK);
    assert(audio_open(s) == H2_AUDIO_OK);
    running = s;
    write_count = 0; removed = disabled = 0;
}
static void assert_sequence(void) {
    const uint8_t expected[][3] = {
        {0,0x32,0}, {0,0x17,0}, {0,0x0e,0xff}, {0,0x12,2},
        {0,0x14,0}, {0,0x0d,0xfa}, {0,0x15,0}, {0,2,0x10},
        {0,0,0}, {0,0,0x1f}, {0,1,0x30}, {0,1,0},
        {0,0x45,0}, {0,0x0d,0xfc}, {0,2,0},
        {1,0x47,0xff}, {1,0x48,0xff}, {1,0x49,0xff}, {1,0x4a,0xff},
        {1,0x4b,0xff}, {1,0x4c,0xff}, {1,0x40,0xc0}, {1,1,0x7f}, {1,6,7},
    };
    assert(write_count == sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < write_count; ++i) {
        assert(writes[i].dev == expected[i][0]);
        assert(writes[i].reg == expected[i][1]);
        assert(writes[i].value == expected[i][2]);
    }
}
int main(void) {
    h2_esp_es8311_es7210_audio_system_t s = {0};
    assert(h2_esp_es8311_es7210_audio_system_power_down(NULL) == H2_AUDIO_ERR_INVALID_ARG);
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    assert(write_count == 0);
    h2_esp_es8311_es7210_audio_system_config_t cfg = config();
    assert(h2_esp_es8311_es7210_audio_system_init(&s, &cfg) == H2_AUDIO_OK);
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    assert(write_count == 0);
    open_system(&s);
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    assert_sequence(); assert(removed == 2 && disabled == 2);
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    assert_sequence();
    /* Register image persists across init: fake reset deliberately clears nothing. */
    open_system(&s);
    assert(registers[0][0x0d] == 1 && registers[0][0x0e] == 2);
    assert(registers[0][1] == 0x3f && registers[0][0x12] == 0);
    assert(registers[0][0x32] == 0xbf && registers[0][0x15] == 0x40);
    assert(registers[1][6] == 0 && registers[1][0x40] == 0x43);
    assert(registers[1][0x47] == 8 && registers[1][0x4b] == 0);
    assert(h2_esp_es8311_es7210_audio_system_deinit(&s) == H2_AUDIO_OK);
    assert_sequence();
    for (int i = 0; i < 24; ++i) {
        open_system(&s); fail_write = i;
        assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_ERR_IO);
        assert_sequence(); assert(removed == 0 && disabled == 0);
        assert(s.es8311 && s.es7210 && s.codec_shutdown_pending);
        assert(audio_open(&s) == H2_AUDIO_ERR_INVALID_STATE);
        fail_write = -1; write_count = 0;
        assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
        assert_sequence();
    }
    open_system(&s); pa_fail = 1;
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_ERR_IO);
    assert(write_count == 0 && removed == 0);
    pa_fail = 0;
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    open_system(&s); s.mic_started = 1; s.mic_task = (void *)7; join_workers = 0;
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) != H2_AUDIO_OK);
    assert(write_count == 0 && removed == 0);
    join_workers = 1;
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    open_system(&s);
    assert(audio_start_speaker(&s) == H2_AUDIO_OK);
    write_count = 0;
    assert(audio_stop_speaker(&s) == H2_AUDIO_OK);
    assert_sequence();
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    assert_sequence();
    open_system(&s);
    assert(audio_start_speaker(&s) == H2_AUDIO_OK);
    write_count = 0; join_workers = 0;
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_ERR_WOULD_BLOCK);
    assert(write_count == 0 && removed == 0 && !pa_enabled);
    join_workers = 1;
    assert(h2_esp_es8311_es7210_audio_system_power_down(&s) == H2_AUDIO_OK);
    assert_sequence();
    puts("power_down tests passed");
    return 0;
}
