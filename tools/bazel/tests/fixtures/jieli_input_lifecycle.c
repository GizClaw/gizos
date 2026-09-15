#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_atomic.h"
enum { IO_PORTB_00, IO_PORTC_09, IO_PORTB_08, IO_PORTC_07, IO_PORTC_08, IO_PORTB_04, IO_PORTB_01 };
enum { EMI_SET_ISR_CB = 10, EMI_USE_SEND_SEM, IOCTL_EMI_WRITE_NON_BLOCK,
    IOCTL_EMI_WRITE_NON_BLOCK_FLUSH, IIC_IOCTL_START, IIC_IOCTL_STOP,
    IIC_IOCTL_TX_WITH_START_BIT, IIC_IOCTL_TX, IIC_IOCTL_TX_WITH_STOP_BIT, IIC_IOCTL_RX_WITH_STOP_BIT };
#define AD_CH_PB01 1u
#define ADC_MAX_CH 10u
static int emi_handle, iic_handle, close_error, flush_error, iic_error, gpio_error;
static int closes, stops, dma_pending, rs, check_rs, stale_flush, lost_irq;
static uint32_t clock_ms;
static void complete_transfer(void);
static atomic_int adc_calls, adc_result, pause_adc, pause_draw, pause_touch, entered, release_worker;
static void pause_operation(void) {
    atomic_store(&entered, 1);
    while (!atomic_load(&release_worker)) sched_yield();
}
static void progress_transfer(void) {
    if (stale_flush && !lost_irq && clock_ms >= 5u && dma_pending) {
        dma_pending = 0;
        complete_transfer();
    }
}
uint32_t timer_get_ms(void) { ++clock_ms; progress_transfer(); return clock_ms; }
static void os_time_dly(uint32_t ticks) { clock_ms += ticks * 10u; progress_transfer(); }
static int gpio_set_direction(int pin, int direction) { (void)direction; return pin == IO_PORTB_01 ? gpio_error : 0; }
static int gpio_set_die(int pin, int enabled) { (void)pin; (void)enabled; return gpio_error; }
static int gpio_set_pull_up(int pin, int enabled) { (void)pin; (void)enabled; return gpio_error; }
static int gpio_set_pull_down(int pin, int enabled) { (void)pin; (void)enabled; return gpio_error; }
static int gpio_read(int pin) { (void)pin; return 0; }
static int gpio_direction_output(int pin, int value) {
    if (pin == IO_PORTC_09) {
        if (check_rs && dma_pending) assert(value == rs);
        rs = value;
    }
    return 0;
}
static void *dev_open(const char *name, void *arg) { (void)arg; return strcmp(name, "emi") == 0 ? &emi_handle : &iic_handle; }
static int dev_close(void *device) {
    assert(device == &emi_handle || device == &iic_handle);
    ++closes;
    return close_error;
}
static int fake_dev_ioctl(void *device, int command, uintptr_t argument) {
    assert(device == &emi_handle || device == &iic_handle);
    if (command == IOCTL_EMI_WRITE_NON_BLOCK_FLUSH) {
        if (flush_error) return -1;
        if (!stale_flush && !lost_irq && dma_pending) {
            dma_pending = 0;
            complete_transfer();
        }
    }
    if (command == IIC_IOCTL_START && atomic_load(&pause_touch)) pause_operation();
    if (command == IIC_IOCTL_STOP) ++stops;
    if (command == IIC_IOCTL_RX_WITH_STOP_BIT) *(uint8_t *)argument = 0x64;
    return command == iic_error ? -1 : 0;
}
#define dev_ioctl(device, command, argument) fake_dev_ioctl(device, command, (uintptr_t)(argument))
static int dev_write(void *device, void *buffer, uint32_t length) {
    assert(device == &emi_handle && buffer != NULL);
    /* Pinned EMI waits for prior DMA and copies <=4 bytes to native storage. */
    dma_pending = 0;
    if (length == 2u && atomic_load(&pause_draw)) pause_operation();
    dma_pending = 1;
    return (int)length;
}
static uint32_t adc_add_sample_ch(uint32_t channel) {
    assert(channel == AD_CH_PB01);
    int call = atomic_fetch_add(&adc_calls, 1);
    if (call == 0 && atomic_load(&pause_adc)) pause_operation();
    return (uint32_t)atomic_load(&adc_result);
}
static uint32_t adc_get_value(uint32_t channel) { assert(channel == AD_CH_PB01); return 0; }
/* PROVIDER */
static void complete_transfer(void) { /* COMPLETE */; }
static void *draw_thread(void *unused) {
    (void)unused;
    uint16_t pixel = 0;
    h2_display_rect_t rect = {0, 0, 1, 1};
    const h2_pal_display_api_t *api = h2_jieli_ac791n_devkit_display_api();
    assert(api->vtable->draw_bitmap(api->user, &rect, &pixel, 2u, H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
    return NULL;
}
static void *touch_thread(void *unused) {
    (void)unused;
    h2_pal_touch_event_t event;
    const h2_pal_touch_api_t *api = h2_jieli_ac791n_devkit_touch_api();
    assert(api->vtable->poll_event(api->user, &event) == H2_PAL_OK);
    return NULL;
}
static void *button_thread(void *unused) {
    (void)unused;
    h2_pal_single_button_reading_t reading;
    assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, &reading) == H2_PAL_OK);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    display_state.device = &emi_handle;
    display_state.open = 1;
    touch_state.iic = &iic_handle;
    touch_state.open = 1;
    const h2_pal_display_api_t *display = h2_jieli_ac791n_devkit_display_api();
    const h2_pal_touch_api_t *touch = h2_jieli_ac791n_devkit_touch_api();
    if (strcmp(argv[1], "invalid_input") == 0) {
        for (int open = 0; open <= 1; ++open) {
            display_state.open = touch_state.open = open;
            assert(display->vtable->get_info(display->user, NULL) == H2_DISPLAY_ERR_INVALID_ARG);
            assert(touch->vtable->get_info(touch->user, NULL) == H2_PAL_ERR_INVALID_ARG);
            assert(touch->vtable->poll_event(touch->user, NULL) == H2_PAL_ERR_INVALID_ARG);
        }
        assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, NULL) == H2_PAL_ERR_INVALID_ARG);
        assert(read_radio_button_group(NULL, H2_JIELI_AC791N_ADKEY_GROUP_ID, NULL) == H2_PAL_ERR_INVALID_ARG);
        assert(read_single_button(NULL, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
        assert(read_radio_button_group(NULL, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
        assert(atomic_load(&adc_calls) == 0);
        h2_display_rect_t rect = {0, 0, 1, 1};
        uint16_t pixel = 0;
        assert(display->vtable->draw_bitmap(display->user, NULL, &pixel, 2, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_INVALID_ARG);
        assert(display->vtable->draw_bitmap(display->user, &rect, NULL, 2, H2_DISPLAY_PIXEL_RGB565) == H2_DISPLAY_ERR_INVALID_ARG);
        assert(display->vtable->set_brightness_percent(display->user, 101) == H2_DISPLAY_ERR_INVALID_ARG);
    } else if (strcmp(argv[1], "flush_failure") == 0) {
        assert(lcd_command(0x2a) == 0);
        flush_error = 1;
        assert(display->vtable->close(display->user) == H2_PAL_ERR_IO);
        assert(display_state.open && display_state.device == &emi_handle && closes == 0);
        flush_error = 0;
        assert(display->vtable->close(display->user) == H2_PAL_OK);
        assert(closes == 1);
    } else if (strcmp(argv[1], "display_close_error") == 0) {
        close_error = 1;
        assert(display->vtable->close(display->user) == H2_PAL_ERR_IO);
        assert(!display_state.open && display_state.device == NULL);
        assert(display->vtable->close(display->user) == H2_PAL_OK && closes == 1);
    } else if (strcmp(argv[1], "touch_close_error") == 0) {
        close_error = 1;
        assert(touch->vtable->close(touch->user) == H2_PAL_ERR_IO);
        assert(!touch_state.open && touch_state.iic == NULL);
        assert(touch->vtable->close(touch->user) == H2_PAL_OK && closes == 1);
    } else if (strcmp(argv[1], "flush_lost_irq") == 0) {
        assert(lcd_command(0x2a) == 0);
        lost_irq = 1;
        assert(display->vtable->close(display->user) == H2_PAL_ERR_IO);
        assert(display_state.device == &emi_handle && display_state.open && closes == 0);
        lost_irq = 0;
        complete_transfer();
        assert(display->vtable->close(display->user) == H2_PAL_OK && closes == 1);
    } else if (strcmp(argv[1], "open_pending") == 0) {
        display_state.device = NULL;
        display_state.open = 0;
        lost_irq = 1;
        assert(display->vtable->open(display->user) == H2_PAL_ERR_IO);
        assert(display_state.device == &emi_handle && !display_state.open && closes == 0);
        assert(display->vtable->open(display->user) == H2_PAL_ERR_INVALID_STATE);
        lost_irq = 0;
        complete_transfer();
        assert(display->vtable->close(display->user) == H2_PAL_OK && closes == 1);
    } else if (strcmp(argv[1], "flush_stale") == 0) {
        check_rs = 1;
        assert(lcd_command(0x2a) == 0);
        stale_flush = 1;
        assert(lcd_data(0x11) == 0);
        assert(clock_ms >= 5u);
    } else if (strcmp(argv[1], "rs_order") == 0) {
        check_rs = 1;
        assert(lcd_command(0x2a) == 0);
        assert(lcd_data(0x11) == 0);
        assert(lcd_command(0x2c) == 0);
    } else if (strncmp(argv[1], "iic_", 4) == 0) {
        iic_error = strstr(argv[1], "start") != NULL ? IIC_IOCTL_START : IIC_IOCTL_STOP;
        if (strstr(argv[1], "read") != NULL) {
            uint8_t value = 0;
            assert(touch_read_register(0, &value) != 0);
        } else {
            assert(touch_write_register(0, 0) != 0);
        }
        assert(stops == 1);
    } else if (strcmp(argv[1], "adc_full") == 0 || strcmp(argv[1], "adc_gpio") == 0) {
        h2_pal_single_button_reading_t reading = {.id = 99};
        adc_result = strcmp(argv[1], "adc_full") == 0 ? ADC_MAX_CH : 0;
        gpio_error = strcmp(argv[1], "adc_gpio") == 0 ? -1 : 0;
        assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, &reading) == H2_PAL_ERR_IO);
        assert(reading.id == 99);
        adc_result = 3;
        gpio_error = 0;
        assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, &reading) == H2_PAL_OK);
        assert(reading.state == H2_PAL_BUTTON_STATE_PRESSED);
    } else {
        pthread_t worker;
        if (strcmp(argv[1], "draw_close") == 0) {
            pause_draw = 1;
            assert(pthread_create(&worker, NULL, draw_thread, NULL) == 0);
        } else if (strcmp(argv[1], "touch_close") == 0) {
            pause_touch = 1;
            assert(pthread_create(&worker, NULL, touch_thread, NULL) == 0);
        } else {
            pause_adc = 1;
            assert(pthread_create(&worker, NULL, button_thread, NULL) == 0);
        }
        while (!atomic_load(&entered)) sched_yield();
        if (pause_draw) {
            assert(display->vtable->close(display->user) == H2_PAL_ERR_BUSY);
        } else if (pause_touch) {
            assert(touch->vtable->close(touch->user) == H2_PAL_ERR_BUSY);
        } else {
            h2_pal_radio_button_group_reading_t reading;
            assert(read_radio_button_group(NULL, H2_JIELI_AC791N_ADKEY_GROUP_ID, &reading) == H2_PAL_ERR_BUSY);
            assert(atomic_load(&adc_calls) == 1);
        }
        atomic_store(&release_worker, 1);
        assert(pthread_join(worker, NULL) == 0);
        if (pause_draw) assert(display->vtable->close(display->user) == H2_PAL_OK);
        if (pause_touch) assert(touch->vtable->close(touch->user) == H2_PAL_OK);
        if (pause_adc) {
            h2_pal_radio_button_group_reading_t reading;
            assert(read_radio_button_group(NULL, H2_JIELI_AC791N_ADKEY_GROUP_ID, &reading) == H2_PAL_OK);
            assert(atomic_load(&adc_calls) == 1);
        }
    }
    return 0;
}
