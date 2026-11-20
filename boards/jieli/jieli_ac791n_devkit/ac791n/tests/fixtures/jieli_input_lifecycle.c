#include <assert.h>
#include <pthread.h>
#include "h2_atomic.h"
#include <stdlib.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_atomic.h"
enum {
  IO_PORTB_00,
  IO_PORTC_09,
  IO_PORTB_08,
  IO_PORTC_07,
  IO_PORTC_08,
  IO_PORTB_04,
  IO_PORTB_01
};
enum {
  EMI_SET_ISR_CB = 10,
  EMI_USE_SEND_SEM,
  IOCTL_EMI_WRITE_NON_BLOCK,
  IOCTL_EMI_WRITE_NON_BLOCK_FLUSH,
  IIC_IOCTL_START,
  IIC_IOCTL_STOP,
  IIC_IOCTL_TX_WITH_START_BIT,
  IIC_IOCTL_TX,
  IIC_IOCTL_TX_WITH_STOP_BIT,
  IIC_IOCTL_RX_WITH_STOP_BIT
};
#define AD_CH_PB01 1u
#define ADC_MAX_CH 10u
static int emi_handle, iic_handle, close_error, flush_error, iic_error,
    gpio_error;
static int closes, stops, dma_pending, rs, check_rs, stale_flush, lost_irq;
static int complete_small, ramwr_flush_error, ramwr_lost_irq;
static int model_panel;
static const uint8_t* dma_source;
static uint32_t dma_length;
static uint8_t small_packet[4];
static uint32_t clock_ms;
static void complete_transfer(void);
static h2_atomic_int_t adc_calls, adc_result, pause_adc, pause_draw,
    pause_touch, entered, release_worker;
static void pause_operation(void) {
  h2_atomic_store(&entered, 1);
  while (!h2_atomic_load(&release_worker)) sched_yield();
}
static void progress_transfer(void) {
  if (stale_flush && !lost_irq && clock_ms >= 5u && dma_pending) {
    dma_pending = 0;
    complete_transfer();
  }
}
uint32_t timer_get_ms(void) {
  ++clock_ms;
  progress_transfer();
  return clock_ms;
}
static void os_time_dly(uint32_t ticks) {
  clock_ms += ticks * 10u;
  progress_transfer();
}
static int gpio_set_direction(int pin, int direction) {
  (void)direction;
  return pin == IO_PORTB_01 ? gpio_error : 0;
}
static int gpio_set_die(int pin, int enabled) {
  (void)pin;
  (void)enabled;
  return gpio_error;
}
static int gpio_set_pull_up(int pin, int enabled) {
  (void)pin;
  (void)enabled;
  return gpio_error;
}
static int gpio_set_pull_down(int pin, int enabled) {
  (void)pin;
  (void)enabled;
  return gpio_error;
}
static int gpio_read(int pin) {
  (void)pin;
  return 0;
}
static int gpio_direction_output(int pin, int value) {
  if (pin == IO_PORTC_09) {
    if (check_rs && dma_pending) assert(value == rs);
    rs = value;
  }
  return 0;
}
static void* dev_open(const char* name, void* arg) {
  (void)arg;
  return strcmp(name, "emi") == 0 ? &emi_handle : &iic_handle;
}
static int dev_close(void* device) {
  assert(device == &emi_handle || device == &iic_handle);
  ++closes;
  return close_error;
}
static int fake_dev_ioctl(void* device, int command, uintptr_t argument) {
  assert(device == &emi_handle || device == &iic_handle);
  if (command == IOCTL_EMI_WRITE_NON_BLOCK_FLUSH) {
    if (flush_error) return -1;
    if (!stale_flush && !lost_irq && dma_pending) {
      dma_pending = 0;
      complete_transfer();
    }
  }
  if (command == IIC_IOCTL_START && h2_atomic_load(&pause_touch))
    pause_operation();
  if (command == IIC_IOCTL_STOP) ++stops;
  if (command == IIC_IOCTL_RX_WITH_STOP_BIT) *(uint8_t*)argument = 0x64;
  return command == iic_error ? -1 : 0;
}
#define dev_ioctl(device, command, argument) \
  fake_dev_ioctl(device, command, (uintptr_t)(argument))
static int dev_write(void* device, void* buffer, uint32_t length) {
  assert(device == &emi_handle && buffer != NULL);
  /* Pinned EMI waits for prior DMA and copies <=4 bytes to native storage. */
  dma_pending = 0;
  if (length == 2u && h2_atomic_load(&pause_draw)) pause_operation();
  dma_pending = 1;
  dma_length = length;
  dma_source = buffer;
  if (length <= sizeof(small_packet)) {
    memcpy(small_packet, buffer, length);
    dma_source = small_packet;
  }
  if (length == 1u && rs == 0 && *(const uint8_t*)buffer == 0x2cu) {
    if (ramwr_flush_error) flush_error = 1;
    if (ramwr_lost_irq) lost_irq = 1;
  }
  if (complete_small && length <= 4u) {
    dma_pending = 0;
    complete_transfer();
  }
  return (int)length;
}
static uint32_t adc_add_sample_ch(uint32_t channel) {
  assert(channel == AD_CH_PB01);
  int call = h2_atomic_fetch_add(&adc_calls, 1);
  if (call == 0 && h2_atomic_load(&pause_adc)) pause_operation();
  return (uint32_t)h2_atomic_load(&adc_result);
}
static uint32_t adc_get_value(uint32_t channel) {
  assert(channel == AD_CH_PB01);
  return 0;
}
/* PROVIDER */
static uint16_t model_pixels[H2_LCD_WIDTH * H2_LCD_HEIGHT];
static uint16_t model_x0, model_x1, model_y0, model_y1;
static uint8_t model_command, model_parameters[4];
static unsigned model_parameter_count, model_cursor;
static void complete_transfer(void) {
  /* An 8080 receiver samples RS with the bytes, not when DMA was submitted.
   * Delay that sampling until completion to expose the permitted late-send
   * schedule. Extra address parameters are ignored until another command. */
  if (model_panel && dma_length != 0u) {
    if (rs == 0) {
      assert(dma_length == 1u);
      model_command = dma_source[0];
      model_parameter_count = model_cursor = 0u;
    } else if (model_command == 0x2au || model_command == 0x2bu) {
      for (uint32_t i = 0u; i < dma_length && model_parameter_count < 4u; ++i)
        model_parameters[model_parameter_count++] = dma_source[i];
      if (model_parameter_count == 4u) {
        uint16_t start = (uint16_t)((uint16_t)model_parameters[0] << 8u) |
                         model_parameters[1];
        uint16_t end = (uint16_t)((uint16_t)model_parameters[2] << 8u) |
                       model_parameters[3];
        if (model_command == 0x2au) {
          model_x0 = start;
          model_x1 = end;
        } else {
          model_y0 = start;
          model_y1 = end;
        }
      }
    } else if (model_command == 0x2cu) {
      assert(dma_length % 2u == 0u && model_x1 >= model_x0 &&
             model_y1 >= model_y0);
      const unsigned width = model_x1 - model_x0 + 1u;
      for (uint32_t i = 0u; i < dma_length; i += 2u) {
        unsigned x = model_x0 + model_cursor % width;
        unsigned y = model_y0 + model_cursor / width;
        assert(x < H2_LCD_WIDTH && y <= model_y1 && y < H2_LCD_HEIGHT);
        model_pixels[y * H2_LCD_WIDTH + x] =
            (uint16_t)((uint16_t)dma_source[i] << 8u) | dma_source[i + 1u];
        ++model_cursor;
      }
    }
    dma_length = 0u;
  }
  /* COMPLETE */;
}
static void* draw_thread(void* unused) {
  (void)unused;
  uint16_t pixel = 0;
  h2_display_rect_t rect = {0, 0, 1, 1};
  const h2_pal_display_api_t* api = h2_jieli_ac791n_devkit_display_api();
  assert(api->vtable->draw_bitmap(api->user, &rect, &pixel, 2u,
                                  H2_DISPLAY_PIXEL_RGB565) == H2_PAL_OK);
  return NULL;
}
static void* touch_thread(void* unused) {
  (void)unused;
  h2_pal_touch_event_t event;
  const h2_pal_touch_api_t* api = h2_jieli_ac791n_devkit_touch_api();
  assert(api->vtable->poll_event(api->user, &event) == H2_PAL_OK);
  return NULL;
}
static void* button_thread(void* unused) {
  (void)unused;
  h2_pal_single_button_reading_t reading;
  assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, &reading) ==
         H2_PAL_OK);
  return NULL;
}
static void h2_fixture_atomic_cleanup(void) {
  h2_atomic_destroy(&adc_calls);
  h2_atomic_destroy(&adc_result);
  h2_atomic_destroy(&pause_adc);
  h2_atomic_destroy(&pause_draw);
  h2_atomic_destroy(&pause_touch);
  h2_atomic_destroy(&entered);
  h2_atomic_destroy(&release_worker);
}
int main(int argc, char** argv) {
  assert(atexit(h2_fixture_atomic_cleanup) == 0);
  assert(h2_atomic_init(&adc_calls, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&adc_result, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&pause_adc, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&pause_draw, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&pause_touch, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&entered, 0) == H2_ATOMIC_OK);
  assert(h2_atomic_init(&release_worker, 0) == H2_ATOMIC_OK);

  assert(argc == 2);
  display_state.device = &emi_handle;
  display_state.open = 1;
  touch_state.iic = &iic_handle;
  touch_state.open = 1;
  const h2_pal_display_api_t* display = h2_jieli_ac791n_devkit_display_api();
  const h2_pal_touch_api_t* touch = h2_jieli_ac791n_devkit_touch_api();
  if (strcmp(argv[1], "invalid_input") == 0) {
    for (int open = 0; open <= 1; ++open) {
      display_state.open = touch_state.open = open;
      assert(display->vtable->get_info(display->user, NULL) ==
             H2_DISPLAY_ERR_INVALID_ARG);
      assert(touch->vtable->get_info(touch->user, NULL) ==
             H2_PAL_ERR_INVALID_ARG);
      assert(touch->vtable->poll_event(touch->user, NULL) ==
             H2_PAL_ERR_INVALID_ARG);
    }
    assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, NULL) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(read_radio_button_group(NULL, H2_JIELI_AC791N_ADKEY_GROUP_ID,
                                   NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(read_single_button(NULL, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(read_radio_button_group(NULL, 0, NULL) == H2_PAL_ERR_INVALID_ARG);
    assert(h2_atomic_load(&adc_calls) == 0);
    h2_display_rect_t rect = {0, 0, 1, 1};
    uint16_t pixel = 0;
    assert(display->vtable->draw_bitmap(display->user, NULL, &pixel, 2,
                                        H2_DISPLAY_PIXEL_RGB565) ==
           H2_DISPLAY_ERR_INVALID_ARG);
    assert(display->vtable->draw_bitmap(display->user, &rect, NULL, 2,
                                        H2_DISPLAY_PIXEL_RGB565) ==
           H2_DISPLAY_ERR_INVALID_ARG);
    assert(display->vtable->set_brightness_percent(display->user, 101) ==
           H2_DISPLAY_ERR_INVALID_ARG);
  } else if (strcmp(argv[1], "flush_failure") == 0) {
    assert(lcd_command(0x2a) == 0);
    flush_error = 1;
    assert(display->vtable->close(display->user) == H2_PAL_ERR_IO);
    assert(display_state.open && display_state.device == &emi_handle &&
           closes == 0);
    flush_error = 0;
    assert(display->vtable->close(display->user) == H2_PAL_OK);
    assert(closes == 1);
  } else if (strcmp(argv[1], "display_close_error") == 0) {
    close_error = 1;
    assert(display->vtable->close(display->user) == H2_PAL_ERR_IO);
    assert(!display_state.open && display_state.device == NULL);
    assert(display->vtable->close(display->user) == H2_PAL_OK && closes == 1);
  } else if (strcmp(argv[1], "pointer_ownership") == 0) {
    bool owned = true;
    assert(h2_pal_touch_open_pointer(touch, &owned) == H2_PAL_ERR_BUSY &&
           !owned);
    assert(h2_pal_touch_close(touch) == H2_PAL_OK);
    assert(h2_pal_touch_open_pointer(touch, &owned) == H2_PAL_OK && owned);
    assert(h2_pal_touch_open(touch) == H2_PAL_ERR_BUSY);
    assert(h2_pal_touch_close(touch) == H2_PAL_ERR_BUSY);
    close_error = 1;
    assert(h2_pal_touch_close_pointer(touch) == H2_PAL_ERR_IO);
    assert(h2_pal_touch_open_pointer(touch, &owned) == H2_PAL_ERR_BUSY &&
           !owned);
    assert(h2_pal_touch_open(touch) == H2_PAL_ERR_BUSY);
    assert(h2_pal_touch_close_pointer(touch) == H2_PAL_OK && closes == 2);
  } else if (strcmp(argv[1], "touch_close_error") == 0) {
    close_error = 1;
    assert(touch->vtable->close(touch->user) == H2_PAL_ERR_IO);
    assert(!touch_state.open && touch_state.iic == NULL);
    assert(touch->vtable->close(touch->user) == H2_PAL_OK && closes == 1);
  } else if (strcmp(argv[1], "flush_lost_irq") == 0) {
    assert(lcd_command(0x2a) == 0);
    lost_irq = 1;
    assert(display->vtable->close(display->user) == H2_PAL_ERR_IO);
    assert(display_state.device == &emi_handle && display_state.open &&
           closes == 0);
    lost_irq = 0;
    complete_transfer();
    assert(display->vtable->close(display->user) == H2_PAL_OK && closes == 1);
  } else if (strcmp(argv[1], "open_pending") == 0 ||
             strcmp(argv[1], "open_flush_error") == 0) {
    display_state.device = NULL;
    display_state.open = 0;
    lost_irq = strcmp(argv[1], "open_pending") == 0;
    flush_error = !lost_irq;
    close_error =
        1; /* Preserve the initialization error even on close failure. */
    assert(display->vtable->open(display->user) == H2_DISPLAY_ERR_IO);
    assert(display_state.device == NULL && !display_state.open && closes == 1);
    assert(!display_state.pending &&
           !h2_jieli_atomic_load_u32(&display_state.dma_done));
    lost_irq = flush_error = close_error = 0;
    assert(display->vtable->open(display->user) == H2_DISPLAY_OK);
    assert(display->vtable->close(display->user) == H2_DISPLAY_OK &&
           closes == 2);
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
  } else if (strncmp(argv[1], "draw_rs_", 8u) == 0) {
    check_rs = 1;
    complete_small = strcmp(argv[1], "draw_rs_immediate") == 0;
    stale_flush = strcmp(argv[1], "draw_rs_stale") == 0;
    ramwr_flush_error = strcmp(argv[1], "draw_rs_flush_error") == 0;
    ramwr_lost_irq = strcmp(argv[1], "draw_rs_lost_irq") == 0;
    uint16_t pixels[4] = {0x1234u, 0x5678u, 0x9abcu, 0xdef0u};
    const uint8_t expected[8] = {0x12u, 0x34u, 0x56u, 0x78u,
                                 0x9au, 0xbcu, 0xdeu, 0xf0u};
    const h2_display_rect_t rect = {2, 3, 2, 2};
    if (ramwr_flush_error || ramwr_lost_irq) {
      memset(display_state.frame, 0x5a, sizeof(expected));
      assert(display->vtable->draw_bitmap(display->user, &rect, pixels, 4u,
                                          H2_DISPLAY_PIXEL_RGB565) ==
             H2_DISPLAY_ERR_IO);
      assert(rs == 0 && dma_pending && display_state.pending &&
             display_state.open && display_state.device == &emi_handle);
      for (size_t i = 0u; i < sizeof(expected); ++i)
        assert(display_state.frame[i] == 0x5a);
      if (ramwr_lost_irq) assert(clock_ms >= 2000u);
      ramwr_flush_error = ramwr_lost_irq = flush_error = lost_irq = 0;
    }
    assert(display->vtable->draw_bitmap(display->user, &rect, pixels, 4u,
                                        H2_DISPLAY_PIXEL_RGB565) ==
           H2_DISPLAY_OK);
    assert(rs == 1 && dma_pending && display_state.pending);
    /* Caller storage may change immediately after draw returns. DMA owns
     * the board staging copy until the following flush/close completes. */
    memset(pixels, 0, sizeof(pixels));
    assert(memcmp(display_state.frame, expected, sizeof(expected)) == 0);
    assert(display->vtable->close(display->user) == H2_DISPLAY_OK);
    assert(!dma_pending && !display_state.open && closes == 1);
  } else if (strncmp(argv[1], "partial_pixels_", 15u) == 0) {
    model_panel = complete_small = 1;
    uint16_t first[4] = {0xf800u, 0xf800u, 0xf800u, 0xf800u};
    const uint16_t second[8] = {0u, 0u, 0x07e0u, 0x07e0u,
                                0u, 0u, 0x07e0u, 0x07e0u};
    const uint16_t third[8] = {0u, 0u, 0x001fu, 0x001fu,
                               0u, 0u, 0x001fu, 0x001fu};
    const h2_display_rect_t a = {0, 0, 2, 2}, b = {0, 0, 4, 2},
                            c = {2, 0, 4, 2};
    assert(display->vtable->draw_bitmap(display->user, &a, first, 4u,
                                        H2_DISPLAY_PIXEL_RGB565) ==
           H2_DISPLAY_OK);
    memset(first, 0, sizeof(first)); /* The caller may immediately reuse it. */
    complete_small = strcmp(argv[1], "partial_pixels_immediate") == 0;
    assert(display->vtable->draw_bitmap(display->user, &b, second, 8u,
                                        H2_DISPLAY_PIXEL_RGB565) ==
           H2_DISPLAY_OK);
    complete_small = 1;
    assert(display->vtable->draw_bitmap(display->user, &c, third, 8u,
                                        H2_DISPLAY_PIXEL_RGB565) ==
           H2_DISPLAY_OK);
    assert(display->vtable->close(display->user) == H2_DISPLAY_OK);
    /* Frame B clears A. Frame C never touches A again: losing B's RAMWR can
     * leave persistent old pixels even after later transfers succeed. */
    for (unsigned y = 0u; y < 2u; ++y) {
      for (unsigned x = 0u; x < 4u; ++x)
        assert(model_pixels[y * H2_LCD_WIDTH + x] == 0u);
      for (unsigned x = 4u; x < 6u; ++x)
        assert(model_pixels[y * H2_LCD_WIDTH + x] == 0x001fu);
    }
  } else if (strncmp(argv[1], "iic_", 4) == 0) {
    iic_error =
        strstr(argv[1], "start") != NULL ? IIC_IOCTL_START : IIC_IOCTL_STOP;
    if (strstr(argv[1], "read") != NULL) {
      uint8_t value = 0;
      assert(touch_read_register(0, &value) != 0);
    } else {
      assert(touch_write_register(0, 0) != 0);
    }
    assert(stops == 1);
  } else if (strcmp(argv[1], "adc_full") == 0 ||
             strcmp(argv[1], "adc_gpio") == 0) {
    h2_pal_single_button_reading_t reading = {.id = 99};
    h2_atomic_store(&adc_result,
                    strcmp(argv[1], "adc_full") == 0 ? ADC_MAX_CH : 0);
    gpio_error = strcmp(argv[1], "adc_gpio") == 0 ? -1 : 0;
    assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, &reading) ==
           H2_PAL_ERR_IO);
    assert(reading.id == 99);
    h2_atomic_store(&adc_result, 3);
    gpio_error = 0;
    assert(read_single_button(NULL, H2_JIELI_AC791N_ADKEY_POWER_ID, &reading) ==
           H2_PAL_OK);
    assert(reading.state == H2_PAL_BUTTON_STATE_PRESSED);
  } else {
    pthread_t worker;
    if (strcmp(argv[1], "draw_close") == 0) {
      h2_atomic_store(&pause_draw, 1);
      assert(pthread_create(&worker, NULL, draw_thread, NULL) == 0);
    } else if (strcmp(argv[1], "touch_close") == 0) {
      h2_atomic_store(&pause_touch, 1);
      assert(pthread_create(&worker, NULL, touch_thread, NULL) == 0);
    } else {
      h2_atomic_store(&pause_adc, 1);
      assert(pthread_create(&worker, NULL, button_thread, NULL) == 0);
    }
    while (!h2_atomic_load(&entered)) sched_yield();
    if (h2_atomic_load(&pause_draw)) {
      assert(display->vtable->close(display->user) == H2_PAL_ERR_BUSY);
    } else if (h2_atomic_load(&pause_touch)) {
      assert(touch->vtable->close(touch->user) == H2_PAL_ERR_BUSY);
    } else {
      h2_pal_radio_button_group_reading_t reading;
      assert(read_radio_button_group(NULL, H2_JIELI_AC791N_ADKEY_GROUP_ID,
                                     &reading) == H2_PAL_ERR_BUSY);
      assert(h2_atomic_load(&adc_calls) == 1);
    }
    h2_atomic_store(&release_worker, 1);
    assert(pthread_join(worker, NULL) == 0);
    if (h2_atomic_load(&pause_draw))
      assert(display->vtable->close(display->user) == H2_PAL_OK);
    if (h2_atomic_load(&pause_touch))
      assert(touch->vtable->close(touch->user) == H2_PAL_OK);
    if (h2_atomic_load(&pause_adc)) {
      h2_pal_radio_button_group_reading_t reading;
      assert(read_radio_button_group(NULL, H2_JIELI_AC791N_ADKEY_GROUP_ID,
                                     &reading) == H2_PAL_OK);
      assert(h2_atomic_load(&adc_calls) == 1);
    }
  }
  return 0;
}
