#include "app_config.h"

#include "asm/emi.h"
#include "asm/iic.h"
#include "asm/includes.h"
#include "asm/sdmmc.h"
#include "device/iic.h"
#include "device/includes.h"
#include "os/os_api.h"
#include "server/audio_dev.h"
#include "system/includes.h"
#include "event/bt_event.h"
#include "h2_jieli_ac791n_devkit.h"
#include "usb/otg.h"
#include "wifi/wifi_connect.h"

#include <string.h>

extern const struct device_operations
    h2_jieli_ac791n_devkit_sd_volume_ops;

const struct wifi_calibration_param wifi_calibration_param = {
    /* Match JieLi's demo_DevKitBoard WL82 RF network.  The UVC/camera
     * board_dev_kit uses a different crystal load and PA matching network. */
    .xosc_l = 0x0b,
    .xosc_r = 0x0b,
    .pa_trim_data = {1, 7, 4, 7, 11, 1, 7},
    .mcs_dgain = {
        45, 45, 45, 42, 60, 60, 75, 70, 62, 52,
        50, 38, 62, 80, 70, 62, 50, 48, 40, 36,
    },
};

/* The SDK's weak accessors prefer calibration values already stored in VM.
 * During development a previous full image can therefore pin another WL82
 * board's RF values even after this board definition is corrected.  This
 * target has a fixed, documented RF network, so make the board values the
 * source of truth for both Bluetooth and Wi-Fi initialization. */
void wifi_get_xosc(u8 *xosc) {
  if (xosc == NULL) return;
  xosc[0] = wifi_calibration_param.xosc_l;
  xosc[1] = wifi_calibration_param.xosc_r;
}

int wifi_get_pa_trim_data(u8 *pa_data) {
  if (pa_data == NULL) return 0;
  memcpy(pa_data, wifi_calibration_param.pa_trim_data,
         sizeof(wifi_calibration_param.pa_trim_data));
  return 1;
}

const struct irq_info irq_info_table[] = {
#ifdef CONFIG_IPMASK_ENABLE
    {IRQ_SOFT5_IDX, 6, 0}, {IRQ_SOFT4_IDX, 6, 1},
#endif
#if CPU_CORE_NUM == 1
    {IRQ_SOFT5_IDX, 7, 0}, {IRQ_SOFT4_IDX, 7, 1}, {-2, -2, -2},
#endif
    {-1, -1, -1},
};

/* UART1 owns PA6: do not also register UART0 on that receive pin. The SDK
 * buffered printf backend and Loader share this one bidirectional device. */
UART1_PLATFORM_DATA_BEGIN(uart1_data).baudrate = H2_JIELI_CONSOLE_BAUD,
    .port = PORT_REMAP, .output_channel = OUTPUT_CHANNEL0,
    .input_channel = INPUT_CHANNEL0,
    .tx_pin = IO_PORTB_03, .rx_pin = IO_PORTA_06,
    .max_continue_recv_cnt = 1024, .idle_sys_clk_cnt = 500000,
    .clk_src = PLL_48M, .flags = UART_RX_USE_DMA, UART1_PLATFORM_DATA_END();

static const struct otg_dev_data otg_data = {
    /* Keep USB code linked for the vendor update library, but UART1 owns the
     * Loader transport and diagnostics.  Do not start the runtime OTG/SOF
     * machinery: the native USB download ROM remains available before this
     * firmware boots. */
    .usb_dev_en = 0x00,
    .slave_online_cnt = 10,
    .slave_offline_cnt = 10,
    .detect_mode = OTG_SLAVE_MODE,
    .detect_time_interval = 50,
};

static const struct emi_platform_data emi_data = {
    .bits_mode = EMI_8BITS_MODE,
    .baudrate = EMI_BAUD_DIV8,
    .colection = EMI_FALLING_COLT,
    .time_out = 1000,
    .th = EMI_TWIDTH_NO_HALF,
    .ts = 0,
    .tw = EMI_BAUD_DIV8,
    .data_bit_en = 0,
};

SW_IIC_PLATFORM_DATA_BEGIN(sw_iic0_data)
    .clk_pin = IO_PORTH_00,
    .dat_pin = IO_PORTH_01,
    .sw_iic_delay = 50,
SW_IIC_PLATFORM_DATA_END()

SD0_PLATFORM_DATA_BEGIN(sd0_data)
    .port = TCFG_SD_PORTS,
    .priority = 3,
    .data_width = TCFG_SD_DAT_WIDTH,
    .speed = TCFG_SD_CLK,
    .detect_mode = TCFG_SD_DET_MODE,
    .detect_func = NULL,
SD0_PLATFORM_DATA_END()

static const struct dac_platform_data dac_data = {
    .sw_differ = 0,
    .pa_auto_mute = 1,
    .pa_mute_port = TCFG_DAC_MUTE_PORT,
    .pa_mute_value = TCFG_DAC_MUTE_VALUE,
    .differ_output = 0,
    .hw_channel = 0x05,
    .ch_num = 4,
    .vcm_init_delay_ms = 1000,
    .mute_delay_ms = 200,
};

static const struct adc_platform_data adc_data = {
    .mic_channel = LADC_CH_MIC1_P_N,
    .mic_ch_num = 1,
    .isel = 2,
    .dump_num = 480,
};

static const struct audio_pf_data audio_pf_data = {
    .adc_pf_data = &adc_data,
    .dac_pf_data = &dac_data,
};

static const struct audio_platform_data audio_data = {
    .private_data = (void *)&audio_pf_data,
};

static const struct low_power_param power_param = {
    .config = TCFG_LOWPOWER_LOWPOWER_SEL,
    .btosc_disable = TCFG_LOWPOWER_BTOSC_DISABLE,
    .vddiom_lev = TCFG_LOWPOWER_VDDIOM_LEVEL,
    .vddiow_lev = TCFG_LOWPOWER_VDDIOW_LEVEL,
    .vdc14_dcdc = TRUE,
    .vdc14_lev = VDC14_VOL_SEL_LEVEL,
    .sysvdd_lev = SYSVDD_VOL_SEL_LEVEL,
    .vlvd_enable = TRUE,
    .vlvd_value = VLVD_SEL_25V,
};

/* JieLi's demo_ble reference initializes a Bluetooth-capable wake source even
 * when only RF_SLEEP_EN is selected.  Keep the same controller power contract
 * on the physical DevKit; PB1 is the reference board's ADC-key wake input. */
static const struct port_wakeup h2_rf_wakeup_port = {
    .edge = FALLING_EDGE,
    .attribute = BLUETOOTH_RESUME,
    .iomap = IO_PORTB_01,
    .low_power = POWER_SLEEP_WAKEUP | POWER_OFF_WAKEUP,
};

static const struct long_press h2_rf_long_press = {
    .enable = FALSE,
    .use_sec4 = TRUE,
    .edge = FALLING_EDGE,
    .iomap = IO_PORTB_01,
};

static const struct sub_wakeup h2_rf_sub_wakeup = {
    .attribute = BLUETOOTH_RESUME,
};

static const struct charge_wakeup h2_rf_charge_wakeup = {
    .attribute = BLUETOOTH_RESUME,
};

static const struct wakeup_param h2_rf_wakeup_param = {
    .port[IO_PORTB_01 / IO_GROUP_NUM] = &h2_rf_wakeup_port,
    .sub = &h2_rf_sub_wakeup,
    .charge = &h2_rf_charge_wakeup,
    .lpres = &h2_rf_long_press,
};

REGISTER_DEVICES(device_table) = {
    {"uart1", &uart_dev_ops, (void *)&uart1_data},
    {"otg", &usb_dev_ops, (void *)&otg_data},
    {"emi", &emi_dev_ops, (void *)&emi_data},
    {"iic0", &iic_dev_ops, (void *)&sw_iic0_data},
    {"sd0", &sd_dev_ops, (void *)&sd0_data},
    /* Keep the SD device graph identical to H2Loader.  The partition wrapper
     * is used by the shared board filesystem implementation when the card has
     * a conventional MBR partition table. */
    {"h2sdv", &h2_jieli_ac791n_devkit_sd_volume_ops, NULL},
    {"audio", &audio_dev_ops, (void *)&audio_data},
    {"rtc", &rtc_dev_ops, NULL},
};

#ifdef CONFIG_DEBUG_ENABLE
void debug_uart_init(void) {
  /* Before the scheduler, putbyte retains logs in the SDK ring. The board
   * starts UART1 after devices_init; its drain and Loader serialize writes. */
}
#endif

void board_early_init(void) {
  dac_early_init(
      0,
      dac_data.differ_output
          ? (dac_data.ch_num > 1 ? 0x0f : 0x03)
          : dac_data.hw_channel,
      dac_data.vcm_init_delay_ms);
  devices_init();
  (void)h2_jieli_ac791n_devkit_console_start();
}
void board_init(void) {
  power_init(&power_param);
  power_keep_state(POWER_KEEP_RESET);
  power_wakeup_init(&h2_rf_wakeup_param);
  adc_init();
  /* JieLi's WL82 board ports load RF/XOSC calibration from the generated
   * config before Bluetooth or Wi-Fi starts.  Without this step BLE can
   * advertise and accept CONNECT_REQ, but fails to track connection anchors. */
  extern void cfg_file_parse(void);
  cfg_file_parse();
}

void app_default_event_handler(struct sys_event *event) {
#ifdef CONFIG_BT_ENABLE
  extern int h2_jieli_ac791n_devkit_ble_bt_event_handler(
      struct sys_event *event);
  if (event != NULL && event->type == SYS_BT_EVENT) {
    (void)h2_jieli_ac791n_devkit_ble_bt_event_handler(event);
  }
#else
  (void)event;
#endif
}
