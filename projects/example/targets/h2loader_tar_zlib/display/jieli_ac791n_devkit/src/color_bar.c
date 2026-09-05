#include "asm/emi.h"
#include "asm/adc_api.h"
#include "asm/iic.h"
#include "asm/includes.h"
#include "asm/system_reset_reason.h"
#include "asm/wdt.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_platform_core.h"
#include "h2_loader_app_client.h"
#include "h2_loader_boot.h"
#include "jieli_h2loader_app_support.h"
#include "device/device.h"
#include "device/iic.h"
#include "device/ioctl_cmds.h"
#include "gpio.h"
#include "os/os_api.h"
#include "system/sys_common.h"
#include "update/dual_bank_updata_api.h"
#include "usb/device/cdc.h"
#include "usb/usb_common_def.h"

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

enum {
  H2_USB_ID = 0,
  H2_LCD_WIDTH = 320,
  H2_LCD_HEIGHT = 480,
  H2_LCD_RESET_PIN = IO_PORTB_00,
  H2_LCD_RS_PIN = IO_PORTC_09,
  H2_LCD_BACKLIGHT_PIN = IO_PORTB_08,
  H2_LCD_CHECK_D6_PIN = IO_PORTC_07,
  H2_LCD_CHECK_D7_PIN = IO_PORTC_08,
  H2_TOUCH_INT_PIN = IO_PORTB_04,
  H2_ADKEY_PIN = IO_PORTB_01,
  H2_FT6236_WRITE = 0x70,
  H2_FT6236_READ = 0x71,
  H2_FT6236_FINGERS = 0x02,
  H2_FT6236_POINT1 = 0x03,
  H2_FT6236_CHIP_ID = 0xa3,
};

typedef enum h2_panel_kind {
  H2_PANEL_ILI9481 = 1,
  H2_PANEL_ILI9488 = 2,
} h2_panel_kind_t;

typedef struct h2_panel_init {
  uint8_t command;
  uint8_t count;
  uint8_t data[12];
} h2_panel_init_t;

static void *lcd_device;
static OS_SEM usb_rx_sem;
static uint8_t lcd_line[H2_LCD_WIDTH * 2u];
static char status_line[64];
static h2_panel_kind_t panel_kind;
static int display_result = -1;
static int strap_d6;
static int strap_d7;
static void *touch_iic;
static uint8_t touch_chip_id;
static int touch_online;
static int touch_down;
static uint16_t touch_x;
static uint16_t touch_y;
static int adkey_active = -1;
static uint16_t adkey_raw;
static int sd_online;
static uint32_t sd_blocks;
static uint32_t sd_block_size;
static h2_loader_app_client_t loader_client;
static uint32_t next_boot_partition = H2_JIELI_PARTITION_APP;
static uint32_t boot_reset_reason;
static h2_pal_fs_api_t loader_fs;

#define H2_JIELI_TRIAL_CHECKSUM_KEY "jieli_trial_checksum"
#define H2_JIELI_TRIAL_RESET_REASON_KEY "jieli_trial_reset_reason"

extern int snprintf(char *buffer, size_t size, const char *format, ...);
extern int vsnprintf(
    char *buffer, size_t size, const char *format, va_list arguments);

static int app_power_get_running(
    void *user, h2_pal_power_boot_partition_t *out_partition) {
  (void)user;
  if (out_partition == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_partition, 0, sizeof(*out_partition));
  out_partition->id = H2_JIELI_PARTITION_APP;
  out_partition->flags = H2_PAL_POWER_BOOT_PARTITION_FLAG_BOOTABLE |
                         H2_PAL_POWER_BOOT_PARTITION_FLAG_RUNNING |
                         H2_PAL_POWER_BOOT_PARTITION_FLAG_APP;
  memcpy(out_partition->name, "app", 4u);
  return H2_PAL_OK;
}

static int app_power_set_next(void *user, uint32_t partition_id) {
  (void)user;
  if (partition_id != H2_JIELI_PARTITION_LOADER &&
      partition_id != H2_JIELI_PARTITION_APP) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  next_boot_partition = partition_id;
  return H2_PAL_OK;
}

static int app_power_reboot(void *user, uint32_t reason) {
  (void)user;
  (void)reason;
  if (next_boot_partition == H2_JIELI_PARTITION_LOADER &&
      flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK) != 0) {
    return H2_PAL_ERR_IO;
  }
  system_reset();
  return H2_PAL_OK;
}

static const h2_pal_power_api_t *app_power_api(void) {
  static const h2_pal_power_vtable_t vtable = {
      .get_running_boot_partition = app_power_get_running,
      .set_next_boot_partition = app_power_set_next,
      .reboot = app_power_reboot,
  };
  static const h2_pal_power_api_t api = {.vtable = &vtable};
  return &api;
}

static int init_loader_client(void) {
  memset(&loader_fs, 0, sizeof(loader_fs));
  int result = h2_jieli_ac791n_devkit_sd_fs_init(&loader_fs);
  if (result != H2_PAL_OK) return result;
  h2_loader_app_client_config_t config;
  result = h2_jieli_app_loader_config_init(
      &config, &loader_fs, app_power_api(),
      (h2_loader_memory_stats_api_t){0}, H2_LOADER_CAPABILITY_UART);
  if (result != H2_PAL_OK) return result;
  return h2_loader_app_client_init(&loader_client, &config);
}

static int enter_trial_boot(void) {
  const h2_pal_pref_api_t *pref = h2_jieli_ac791n_devkit_pref_api();
  const h2_pal_mem_api_t *allocator = h2_jieli_wl82_platform_mem_api();
  h2_loader_status_t status;
  int result = h2_loader_read_pref_status(pref, allocator, &status);
  if (result != H2_PAL_OK) return result;
  if (!status.stage.valid || !status.partition_2.valid ||
      status.partition_2.role != H2_LOADER_IMAGE_ROLE_APP ||
      !h2_loader_metadata_image_equal(&status.stage, &status.partition_2)) {
    return H2_PAL_OK;
  }
  if (status.partition_2.image_checksum[0] == '\0') {
    return H2_PAL_ERR_INVALID_STATE;
  }

  h2_pal_pref_namespace_t *name_space = NULL;
  result = h2_pal_pref_open(
      pref, H2_LOADER_PREF_NAMESPACE, H2_PAL_PREF_OPEN_READ_WRITE,
      &name_space);
  char *previous_checksum = NULL;
  int repeated_trial = 0;
  if (result == H2_PAL_OK) {
    result = name_space->get_string(
        name_space, allocator, H2_JIELI_TRIAL_CHECKSUM_KEY,
        &previous_checksum);
    if (result == H2_PAL_ERR_NOT_FOUND) result = H2_PAL_OK;
  }
  if (result == H2_PAL_OK && previous_checksum != NULL) {
    repeated_trial =
        strcmp(previous_checksum, status.partition_2.image_checksum) == 0;
  }
  if (previous_checksum != NULL) {
    h2_pal_mem_free(allocator, previous_checksum);
  }
  if (result == H2_PAL_OK && !repeated_trial) {
    result = name_space->set_string(
        name_space, H2_JIELI_TRIAL_CHECKSUM_KEY,
        status.partition_2.image_checksum);
  }
  if (result == H2_PAL_OK && repeated_trial) {
    result = name_space->set_u32(
        name_space, H2_JIELI_TRIAL_RESET_REASON_KEY,
        boot_reset_reason);
  }
  if (result == H2_PAL_OK && name_space->commit != NULL) {
    result = name_space->commit(name_space);
  }
  if (name_space != NULL && name_space->close != NULL) {
    int close_result = name_space->close(name_space);
    if (result == H2_PAL_OK) result = close_result;
  }
  if (result != H2_PAL_OK || !repeated_trial) return result;

  if (flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK) != 0) {
    return H2_PAL_ERR_IO;
  }
  system_reset();
  return H2_PAL_ERR_INVALID_STATE;
}

static void lcd_delay_ms(uint32_t ms);

static void usb_cdc_wakeup(struct usb_device_t *usb_device) {
  (void)usb_device;
  os_sem_post(&usb_rx_sem);
}

static void usb_write_text(const char *text) {
  (void)cdc_write_data(
      H2_USB_ID, (uint8_t *)text, (uint32_t)strlen(text));
}

static void usb_write_status(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  int length = vsnprintf(status_line, sizeof(status_line), format, arguments);
  va_end(arguments);
  if (length > 0 && (size_t)length < sizeof(status_line)) {
    usb_write_text(status_line);
  }
}

static int touch_write_register(uint8_t reg, uint8_t value) {
  int result = 0;
  dev_ioctl(touch_iic, IIC_IOCTL_START, 0);
  if (dev_ioctl(touch_iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_WRITE) ||
      dev_ioctl(touch_iic, IIC_IOCTL_TX, reg) ||
      dev_ioctl(touch_iic, IIC_IOCTL_TX_WITH_STOP_BIT, value)) {
    result = -1;
  }
  dev_ioctl(touch_iic, IIC_IOCTL_STOP, 0);
  return result;
}

static int touch_read_register(uint8_t reg, uint8_t *value) {
  int result = 0;
  dev_ioctl(touch_iic, IIC_IOCTL_START, 0);
  if (dev_ioctl(touch_iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_WRITE) ||
      dev_ioctl(touch_iic, IIC_IOCTL_TX, reg) ||
      dev_ioctl(touch_iic, IIC_IOCTL_TX_WITH_START_BIT, H2_FT6236_READ) ||
      dev_ioctl(touch_iic, IIC_IOCTL_RX_WITH_STOP_BIT, (uint32_t)value)) {
    result = -1;
  }
  dev_ioctl(touch_iic, IIC_IOCTL_STOP, 0);
  return result;
}

static void init_touch(void) {
  gpio_set_direction(H2_TOUCH_INT_PIN, 1);
  gpio_set_pull_up(H2_TOUCH_INT_PIN, 1);
  gpio_set_pull_down(H2_TOUCH_INT_PIN, 0);
  touch_iic = dev_open("iic0", NULL);
  if (touch_iic == NULL) return;
  lcd_delay_ms(20u);
  if (touch_read_register(H2_FT6236_CHIP_ID, &touch_chip_id) != 0) return;
  touch_online = touch_chip_id == UINT8_C(0x64);
  if (touch_online) {
    (void)touch_write_register(0x00, 0x00);
    (void)touch_write_register(0x80, 22);
    (void)touch_write_register(0x88, 13);
  }
}

static void poll_touch(void) {
  uint8_t fingers = 0;
  uint8_t point[4];
  if (!touch_online ||
      touch_read_register(H2_FT6236_FINGERS, &fingers) != 0) {
    return;
  }
  if ((fingers & 0x0fu) != 0u) {
    for (size_t i = 0u; i < sizeof(point); ++i) {
      if (touch_read_register((uint8_t)(H2_FT6236_POINT1 + i), &point[i]) != 0) {
        return;
      }
    }
    uint16_t x = (uint16_t)(((uint16_t)(point[0] & 0x0fu) << 8u) | point[1]);
    uint16_t y = (uint16_t)(((uint16_t)(point[2] & 0x0fu) << 8u) | point[3]);
    if (!touch_down) {
      touch_down = 1;
      touch_x = x;
      touch_y = y;
      usb_write_status("JIELI_TOUCH action=down x=%u y=%u\r\n", x, y);
    } else if ((x > touch_x ? x - touch_x : touch_x - x) >= 2u ||
               (y > touch_y ? y - touch_y : touch_y - y) >= 2u) {
      touch_x = x;
      touch_y = y;
      usb_write_status("JIELI_TOUCH action=move x=%u y=%u\r\n", x, y);
    }
  } else if (touch_down) {
    touch_down = 0;
    usb_write_status(
        "JIELI_TOUCH action=up x=%u y=%u\r\n", touch_x, touch_y);
  }
}

static int decode_adkey(uint16_t value) {
  enum { UPLOAD_R = 22, ADC_FULL = 0x3ff };
  static const uint16_t thresholds[] = {
      ((0) + (ADC_FULL * 3 / (3 + UPLOAD_R))) / 2,
      ((ADC_FULL * 3 / (3 + UPLOAD_R)) +
       (ADC_FULL * 75 / (75 + UPLOAD_R * 10))) / 2,
      ((ADC_FULL * 75 / (75 + UPLOAD_R * 10)) +
       (ADC_FULL * 13 / (13 + UPLOAD_R))) / 2,
      ((ADC_FULL * 13 / (13 + UPLOAD_R)) +
       (ADC_FULL * 22 / (22 + UPLOAD_R))) / 2,
      ((ADC_FULL * 22 / (22 + UPLOAD_R)) +
       (ADC_FULL * 36 / (36 + UPLOAD_R))) / 2,
      ((ADC_FULL * 36 / (36 + UPLOAD_R)) +
       (ADC_FULL * 62 / (62 + UPLOAD_R))) / 2,
      ((ADC_FULL * 62 / (62 + UPLOAD_R)) +
       (ADC_FULL * 150 / (150 + UPLOAD_R))) / 2,
      ((ADC_FULL * 150 / (150 + UPLOAD_R)) + ADC_FULL) / 2 + 50,
  };
  for (size_t i = 0u; i < sizeof(thresholds) / sizeof(thresholds[0]); ++i) {
    if (value <= thresholds[i]) return (int)i;
  }
  return -1;
}

static const char *adkey_name(int key) {
  static const char *const names[] = {
      "power", "enc", "photo", "ok", "vol+", "vol-", "mode", "cancel"};
  return key >= 0 && key < 8 ? names[key] : "none";
}

static void init_adkeys(void) {
  gpio_set_die(H2_ADKEY_PIN, 0);
  gpio_set_direction(H2_ADKEY_PIN, 1);
  gpio_set_pull_up(H2_ADKEY_PIN, 0);
  gpio_set_pull_down(H2_ADKEY_PIN, 0);
  (void)adc_add_sample_ch(AD_CH_PB01);
}

static void poll_adkeys(void) {
  static int candidate = -1;
  static uint8_t stable_count;
  uint16_t value = (uint16_t)adc_get_value(AD_CH_PB01);
  int key = decode_adkey(value);
  adkey_raw = value;
  if (key != candidate) {
    candidate = key;
    stable_count = 1u;
    return;
  }
  if (stable_count < 3u) {
    ++stable_count;
    if (stable_count < 3u) return;
  }
  if (key == adkey_active) return;
  if (adkey_active >= 0) {
    usb_write_status(
        "JIELI_ADKEY action=up key=%d name=%s adc=%u\r\n",
        adkey_active, adkey_name(adkey_active), value);
  }
  adkey_active = key;
  if (key >= 0) {
    usb_write_status(
        "JIELI_ADKEY action=down key=%d name=%s adc=%u\r\n",
        key, adkey_name(key), value);
  }
}

static void probe_sd_card(void) {
  uint32_t status = 0;
  void *sd = dev_open("sd0", NULL);
  if (sd == NULL) return;
  (void)dev_ioctl(sd, IOCTL_GET_STATUS, (uint32_t)&status);
  if (status) {
    (void)dev_ioctl(sd, IOCTL_GET_CAPACITY, (uint32_t)&sd_blocks);
    (void)dev_ioctl(sd, IOCTL_GET_BLOCK_SIZE, (uint32_t)&sd_block_size);
    sd_online = sd_blocks != 0u && sd_block_size != 0u;
  }
  dev_close(sd);
}

static int lcd_write(uint8_t value) {
  return dev_write(lcd_device, &value, 1u) == 1 ? 0 : -1;
}

static int lcd_command(uint8_t command) {
  gpio_direction_output(H2_LCD_RS_PIN, 0);
  return lcd_write(command);
}

static int lcd_data(uint8_t value) {
  gpio_direction_output(H2_LCD_RS_PIN, 1);
  return lcd_write(value);
}

static void lcd_delay_ms(uint32_t ms) {
  os_time_dly((ms + 9u) / 10u);
}

static int run_init_sequence(
    const h2_panel_init_t *sequence, size_t count) {
  for (size_t i = 0u; i < count; ++i) {
    if (sequence[i].command == UINT8_C(0x45)) {
      lcd_delay_ms(sequence[i].count);
      continue;
    }
    if (lcd_command(sequence[i].command) != 0) return -1;
    for (uint8_t j = 0u; j < sequence[i].count; ++j) {
      if (lcd_data(sequence[i].data[j]) != 0) return -1;
    }
  }
  return 0;
}

static h2_panel_kind_t detect_panel(void) {
  gpio_set_direction(H2_LCD_CHECK_D6_PIN, 1);
  gpio_set_direction(H2_LCD_CHECK_D7_PIN, 1);
  strap_d6 = gpio_read(H2_LCD_CHECK_D6_PIN) ? 1 : 0;
  strap_d7 = gpio_read(H2_LCD_CHECK_D7_PIN) ? 1 : 0;
  return strap_d6 && strap_d7 ? H2_PANEL_ILI9488 : H2_PANEL_ILI9481;
}

static void reset_panel(void) {
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 1);
  gpio_direction_output(H2_LCD_RS_PIN, 1);
  gpio_direction_output(H2_LCD_RESET_PIN, 1);
  lcd_delay_ms(60u);
  gpio_direction_output(H2_LCD_RESET_PIN, 0);
  lcd_delay_ms(10u);
  gpio_direction_output(H2_LCD_RESET_PIN, 1);
  lcd_delay_ms(100u);
}

static int init_ili9481(void) {
  static const h2_panel_init_t sequence[] = {
      {0x01, 0, {0}}, {0x45, 200, {0}}, {0x13, 1, {0x00}},
      {0x35, 1, {0x00}}, {0x44, 2, {0x01, 0x50}},
      {0xc5, 1, {0x04}}, {0xc5, 1, {0x07}}, {0xe4, 1, {0xa0}},
      {0xd0, 3, {0x05, 0x40, 0x08}},
      {0xd1, 3, {0x00, 0x00, 0x10}}, {0xd2, 2, {0x01, 0x00}},
      {0xc0, 5, {0x00, 0x3b, 0x00, 0x02, 0x11}},
      {0xc8, 12, {0x00, 0x26, 0x21, 0x00, 0x00, 0x1f,
                  0x65, 0x23, 0x77, 0x00, 0x0f, 0x00}},
      {0x3a, 1, {0x55}}, {0x36, 1, {0x4c}}, {0x11, 0, {0}},
      {0x45, 200, {0}}, {0x29, 0, {0}},
  };
  return run_init_sequence(sequence, sizeof(sequence) / sizeof(sequence[0]));
}

static int init_ili9488(void) {
  static const h2_panel_init_t sequence[] = {
      {0x21, 1, {0x00}}, {0x13, 1, {0x00}}, {0x35, 1, {0x00}},
      {0xb1, 2, {0x90, 0x11}}, {0x36, 1, {0xd8}},
      {0xc1, 1, {0x41}}, {0xf7, 4, {0xa9, 0x51, 0x2c, 0x82}},
      {0xc0, 2, {0x0f, 0x0f}}, {0xc2, 1, {0x22}},
      {0xc5, 3, {0x00, 0x53, 0x80}}, {0xb4, 1, {0x02}},
      {0xb7, 1, {0xc6}}, {0xb6, 2, {0x02, 0x42}},
      {0xbe, 2, {0x00, 0x04}}, {0xe9, 1, {0x00}},
      {0x3a, 1, {0x55}}, {0x11, 0, {0}}, {0x45, 200, {0}},
      {0x29, 0, {0}},
  };
  return run_init_sequence(sequence, sizeof(sequence) / sizeof(sequence[0]));
}

static int set_window(void) {
  static const uint8_t x_range[] = {0x00, 0x00, 0x01, 0x3f};
  static const uint8_t y_range[] = {0x00, 0x00, 0x01, 0xdf};
  if (lcd_command(0x2a) != 0) return -1;
  for (size_t i = 0u; i < sizeof(x_range); ++i) {
    if (lcd_data(x_range[i]) != 0) return -1;
  }
  if (lcd_command(0x2b) != 0) return -1;
  for (size_t i = 0u; i < sizeof(y_range); ++i) {
    if (lcd_data(y_range[i]) != 0) return -1;
  }
  return lcd_command(0x2c);
}

static int draw_color_bars(void) {
  static const uint16_t colors[] = {
      UINT16_C(0xffff), UINT16_C(0xffe0), UINT16_C(0x07ff),
      UINT16_C(0x07e0), UINT16_C(0xf81f), UINT16_C(0xf800),
      UINT16_C(0x001f), UINT16_C(0x0000),
  };
  for (uint32_t x = 0u; x < H2_LCD_WIDTH; ++x) {
    uint16_t color = colors[x / (H2_LCD_WIDTH / 8u)];
    lcd_line[x * 2u] = (uint8_t)(color >> 8u);
    lcd_line[x * 2u + 1u] = (uint8_t)color;
  }
  gpio_direction_output(H2_LCD_RS_PIN, 1);
  for (uint32_t y = 0u; y < H2_LCD_HEIGHT; ++y) {
    if (dev_write(lcd_device, lcd_line, sizeof(lcd_line)) !=
        (int)sizeof(lcd_line)) {
      return -1;
    }
  }
  return 0;
}

static int display_color_bar(void) {
  panel_kind = detect_panel();
  lcd_device = dev_open("emi", NULL);
  if (lcd_device == NULL) return -1;
  reset_panel();
  if ((panel_kind == H2_PANEL_ILI9488 ? init_ili9488() : init_ili9481()) != 0) {
    return -2;
  }
  if (set_window() != 0 || draw_color_bars() != 0) return -3;
  gpio_direction_output(H2_LCD_BACKLIGHT_PIN, 0);
  return 0;
}

static void report_status(void) {
  int length = snprintf(
      status_line, sizeof(status_line),
      "JIELI_COLOR_BAR panel=%s straps=%d%d result=%s\r\n",
      panel_kind == H2_PANEL_ILI9488 ? "ili9488" : "ili9481",
      strap_d6, strap_d7, display_result == 0 ? "ok" : "error");
  if (length > 0 && (size_t)length < sizeof(status_line)) {
    usb_write_text(status_line);
  }
  length = snprintf(
      status_line, sizeof(status_line),
      "JIELI_COLOR_BAR size=320x480 code=%d\r\n", display_result);
  if (length > 0 && (size_t)length < sizeof(status_line)) {
    usb_write_text(status_line);
  }
  usb_write_status(
      "JIELI_TOUCH controller=ft6236 online=%d chip_id=0x%02x\r\n",
      touch_online, touch_chip_id);
  usb_write_status(
      "JIELI_ADKEY count=8 pin=PB1 adc=%u active=%d\r\n",
      adkey_raw, adkey_active);
  uint32_t sd_mib = sd_block_size == 0u
                        ? 0u
                        : (uint32_t)(((uint64_t)sd_blocks * sd_block_size) >> 20u);
  usb_write_status(
      "JIELI_SD online=%d blocks=%u block_size=%u size_mib=%u\r\n",
      sd_online, sd_blocks, sd_block_size, sd_mib);
  usb_write_status(
      "JIELI_BOOT reset_reason=0x%x trial_guard=armed\r\n",
      (unsigned)boot_reset_reason);
}

void app_main(void) {
  uint8_t request[64];
  boot_reset_reason = system_reset_reason_get();
  int trial_result = enter_trial_boot();
  if (trial_result != H2_PAL_OK) {
    /* A trial whose durable state cannot be read or armed is unsafe. Return
     * to the still-valid Loader bank instead of boot-looping in the App. */
    (void)flash_update_clr_boot_info(CLEAR_APP_RUNNING_BANK);
    system_reset();
    return;
  }
  /* Keep the screen as an independent boot indicator.  USB may fail before a
   * CDC log channel exists, so never gate panel initialization on USB. */
  display_result = display_color_bar();
  int loader_result = init_loader_client();
  if (os_sem_create(&usb_rx_sem, 0) != OS_NO_ERR) return;
  if (h2_jieli_ac791n_devkit_usb_debug_start() != H2_PAL_OK) return;
  cdc_set_wakeup_handler(H2_USB_ID, usb_cdc_wakeup);
  init_touch();
  init_adkeys();
  lcd_delay_ms(100u);
  probe_sd_card();
  for (;;) {
    if (os_sem_pend(&usb_rx_sem, 2u) == OS_NO_ERR) {
      uint32_t received = cdc_read_data(H2_USB_ID, request, sizeof(request));
      if (received >= 4u && memcmp(request, "PING", 4u) == 0) {
        report_status();
      }
      if (received >= 7u && memcmp(request, "CONFIRM", 7u) == 0) {
        int result = loader_result == H2_PAL_OK
                         ? h2_jieli_app_loader_confirm(&loader_client.config)
                         : loader_result;
        usb_write_status(
            "JIELI_APP_CONFIRM result=%s code=%d\r\n",
            result == H2_PAL_OK ? "OK" : "fail", result);
      }
      if (received >= 8u && memcmp(request, "RECOVERY", 8u) == 0) {
        int result = loader_result == H2_PAL_OK
                         ? h2_loader_reboot_h2loader_with_transition(
                               &loader_client.loader, NULL, NULL)
                         : loader_result;
        usb_write_status(
            "JIELI_RECOVERY result=%d action=%s\r\n", result,
            result == H2_PAL_OK ? "return-to-loader" : "none");
      }
    }
    poll_touch();
    poll_adkeys();
    wdt_clear();
  }
}
