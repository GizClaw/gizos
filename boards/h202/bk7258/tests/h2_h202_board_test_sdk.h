#ifndef H2_H202_BOARD_TEST_SDK_H
#define H2_H202_BOARD_TEST_SDK_H
#include "h2/pal/hal/h2_pal_wifi.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef unsigned gpio_id_t;
typedef struct {
  int io_mode, pull_mode, func_mode;
} gpio_config_t;
enum {
  GPIO_OUTPUT_ENABLE = 1,
  GPIO_PULL_DISABLE = 0,
  GPIO_SECOND_FUNC_DISABLE = 0
};
enum {
  GPIO_0 = 0,
  GPIO_1 = 1,
  GPIO_4 = 4,
  GPIO_7 = 7,
  GPIO_13 = 13,
  GPIO_22 = 22,
  GPIO_23 = 23,
  GPIO_24 = 24,
  GPIO_25 = 25,
  GPIO_26 = 26,
  GPIO_27 = 27,
  GPIO_37 = 37,
  GPIO_53 = 53,
  GPIO_54 = 54,
  GPIO_SECOND_FUNC_ENABLE = 1,
  GPIO_IO_DISABLE = 0,
  GPIO_PULL_UP_EN = 1,
  GPIO_INT_DISABLE = 0,
  GPIO_INT_TYPE_LOW_LEVEL = 0,
  GPIO_LOW_POWER_DISCARD_IO_STATUS = 0,
  GPIO_DRIVER_CAPACITY_0 = 0,
  GPIO_DRIVER_CAPACITY_3 = 3,
  GPIO_INIT_ENABLE = 1,
  GPIO_TIME_SHARING_MULTIPLEX_DISABLE = 0,
  GPIO_DEV_INVALID = -1,
  GPIO_DEV_UART1_TXD = 1,
  GPIO_DEV_UART1_RXD,
  GPIO_DEV_QSPI0_CLK,
  GPIO_DEV_QSPI0_CSN,
  GPIO_DEV_QSPI0_IO0,
  GPIO_DEV_QSPI0_IO1,
  GPIO_DEV_QSPI0_IO2,
  GPIO_DEV_QSPI0_IO3
};
int gpio_dev_unmap(gpio_id_t pin);
int bk_gpio_set_config(gpio_id_t pin, const gpio_config_t* config);
int bk_gpio_set_output_high(gpio_id_t pin);
int bk_gpio_set_output_low(gpio_id_t pin);
int bk_wifi_sta_get_mac(uint8_t mac[6]);

#define VFS_QSPI_FLASH_0_PATITION_0 "."
enum {
  LFS_QSPI_FLASH = 1,
  LFS_ERR_OK = 0,
  LFS_ERR_NOENT = -2,
  LFS_ERR_NOATTR = -61,
  LFS_ERR_NOMEM = -12,
  LFS_ERR_NOSPC = -28,
  LFS_ERR_FBIG = -27,
  LFS_ERR_INVAL = -22,
  LFS_ERR_EXIST = -17
};
struct bk_little_fs_partition {
  int part_type;
  const char* mount_path;
  struct {
    unsigned start_addr, size;
  } part_flash;
};
int bk_vfs_mount(const char* source, const char* path, const char* type,
                 unsigned flags, void* partition);
int bk_vfs_umount(const char* path);
int bk_vfs_mkdir(const char* path, unsigned mode);
int bk_vfs_stat(const char* path, struct stat* info);
int h2_h202_test_open(const char* path, int flags);
#define bk_vfs_open h2_h202_test_open
#define bk_vfs_read read
#define bk_vfs_write write
#define bk_vfs_lseek lseek
#define bk_vfs_close close
#define bk_vfs_fsync fsync
#define bk_vfs_unlink unlink
#define bk_vfs_rename rename
#define bk_vfs_opendir opendir
struct dirent* h2_h202_test_readdir(DIR* dir);
#define bk_vfs_readdir h2_h202_test_readdir
#define bk_vfs_closedir closedir
int h2_h202_test_rmdir(const char* path);
#define bk_vfs_rmdir h2_h202_test_rmdir
#define os_printf printf
#define os_malloc malloc
#define os_free free

#ifdef H2_H202_MFG_TEST
#include "h2_runtime.h"
enum {
  GPIO_INPUT_ENABLE = 2,
  GPIO_PULL_DOWN_EN = 2,
  GPIO_DEV_I2C1_SCL = 30,
  GPIO_DEV_I2C1_SDA = 31,
  SPI_ROLE_MASTER = 0,
  SPI_BIT_WIDTH_8BITS = 8,
  SPI_POLARITY_LOW = 0,
  SPI_PHASE_1ST_EDGE = 0,
  SPI_4WIRE_MODE = 4,
  SPI_MSB_FIRST = 0,
  SPI_DMA_MODE_DISABLE = 0,
  PM_MODE_DEEP_SLEEP = 2,
  PM_MODE_DEFAULT = 3,
  H2_BK_NFC_FM175XX_DEFAULT_READ_CAPACITY = 256,
};
typedef unsigned spi_id_t;
typedef void (*spi_isr_t)(spi_id_t, void*);
typedef void* beken_semaphore_t;
typedef struct {
  unsigned role, bit_width, polarity, phase, wire_mode, baud_rate, bit_order,
      dma_mode;
} spi_config_t;
typedef struct {
  h2_pal_nfc_api_t api;
} h2_bk_nfc_fm175xx_t;
typedef struct {
  unsigned id, i2c_id, i2c_address, i2c_speed_hz;
  size_t read_capacity;
} h2_bk_nfc_fm175xx_config_t;
int bk_gpio_get_input(gpio_id_t id);
uint32_t bk_gpio_get_value(gpio_id_t id);
int bk_gpio_set_value(gpio_id_t id, uint32_t value);
int gpio_dev_map(gpio_id_t id, unsigned function);
int bk_gpio_register_wakeup_source(gpio_id_t id, unsigned level);
int bk_gpio_register_lowpower_keep_status(gpio_id_t id,
                                          const gpio_config_t* config);
int bk_pm_ap_sleep_mode_set(unsigned mode);
int bk_gpio_unregister_wakeup_source(gpio_id_t id);
int bk_gpio_unregister_lowpower_keep_status(gpio_id_t id);
void rtos_delay_milliseconds(unsigned ms);
int rtos_init_semaphore(beken_semaphore_t* semaphore, unsigned count);
int rtos_set_semaphore(beken_semaphore_t* semaphore);
int rtos_get_semaphore(beken_semaphore_t* semaphore, unsigned timeout);
int rtos_deinit_semaphore(beken_semaphore_t* semaphore);
int bk_spi_driver_init(void);
int bk_spi_init(spi_id_t id, const spi_config_t* config);
int bk_spi_deinit(spi_id_t id);
int bk_spi_register_tx_finish_isr(spi_id_t id, spi_isr_t callback, void* user);
int bk_spi_write_bytes_async(spi_id_t id, const void* data, unsigned size);
h2_pal_result_t h2_bk_nfc_fm175xx_init(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config);
const h2_pal_nfc_api_t* h2_bk_nfc_fm175xx_api(h2_bk_nfc_fm175xx_t* adapter);
#endif

#ifdef H2_H202_AUDIO_HOST_TEST
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* Minimal SDK seam, implemented by test_audio.c. Pointer arguments to the
 * 32-bit target AEC API use uintptr_t here so host tests retain full addresses.
 */
typedef uint32_t TickType_t;
typedef pthread_mutex_t* beken_mutex_t;
typedef struct test_audio_port* audio_port_handle_t;
typedef struct test_audio_element* audio_element_handle_t;
typedef int (*port_stream_func)(audio_port_handle_t, char*, int, TickType_t,
                                void*);
typedef struct {
  port_stream_func write;
} audio_port_cfg_t;
typedef struct {
  int ringbuf_size;
} ringbuf_port_cfg_t;
enum {
  PORT_TYPE_RB = 1,
  PORT_TYPE_CB = 3,
  AEL_IO_FAIL = -1,
  AEL_IO_TIMEOUT = -4
};
#define BK_MS_TO_TICKS(ms) (ms)

typedef struct {
  struct {
    unsigned sample_rate, chl_num;
  } adc_cfg;
  unsigned frame_size, out_block_size, out_block_num;
  unsigned dual_dmic, dual_dmic_sgl_out, ref_mode, multi_out_port_num;
  unsigned task_stack, task_core, task_prio;
} onboard_dual_dmic_mic_stream_cfg_t;
#define DEFAULT_ONBOARD_DUAL_DMIC_STREAM_CONFIG() \
  ((onboard_dual_dmic_mic_stream_cfg_t){0})
typedef struct {
  unsigned sample_rate, frame_size, task_stack, task_core, task_prio;
  bool pa_ctrl_en;
  unsigned pa_ctrl_gpio, pa_on_level, pa_on_delay, pa_off_delay;
} onboard_speaker_stream_cfg_t;
#define DEFAULT_ONBOARD_SPEAKER_STREAM_CONFIG() \
  ((onboard_speaker_stream_cfg_t){0})

typedef struct {
  int16_t ref[H2_H202_AUDIO_FRAME_SAMPLES];
  int16_t mic[H2_H202_AUDIO_FRAME_SAMPLES];
  int16_t out[H2_H202_AUDIO_FRAME_SAMPLES];
} AECContext;
enum {
  AEC_CTRL_CMD_GET_FRAME_SAMPLE,
  AEC_CTRL_CMD_GET_RX_BUF,
  AEC_CTRL_CMD_GET_TX_BUF,
  AEC_CTRL_CMD_GET_OUT_BUF,
  AEC_CTRL_CMD_SET_FLAGS,
  AEC_CTRL_CMD_SET_MIC_DELAY
};
unsigned aec_size(unsigned samples);
void aec_init(AECContext* aec, unsigned rate);
void aec_ctrl(AECContext* aec, uint32_t cmd, uintptr_t arg);
void aec_proc(AECContext* aec, int16_t* ref, int16_t* mic, int16_t* out);
int bk_audio_osi_funcs_init(void);
const h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void);
const h2_pal_time_api_t* h2_bk_platform_time_api(void);
int rtos_init_mutex(beken_mutex_t* mutex);
int rtos_lock_mutex(beken_mutex_t* mutex);
int rtos_unlock_mutex(beken_mutex_t* mutex);
int rtos_deinit_mutex(beken_mutex_t* mutex);
void rtos_delay_milliseconds(unsigned ms);
audio_port_handle_t audio_port_init(audio_port_cfg_t* config);
audio_port_handle_t ringbuf_port_init(ringbuf_port_cfg_t* config);
int audio_port_set_type(audio_port_handle_t port, int type);
int audio_port_deinit(audio_port_handle_t port);
int audio_port_write(audio_port_handle_t port, char* buffer, int bytes,
                     TickType_t ticks);
int audio_port_get_filled_size(audio_port_handle_t port);
audio_element_handle_t onboard_dual_dmic_mic_stream_init(
    onboard_dual_dmic_mic_stream_cfg_t* config);
audio_element_handle_t onboard_speaker_stream_init(
    onboard_speaker_stream_cfg_t* config);
int audio_element_set_output_port(audio_element_handle_t el,
                                  audio_port_handle_t port);
int audio_element_set_multi_output_port(audio_element_handle_t el,
                                        audio_port_handle_t port, int index);
int audio_element_set_input_port(audio_element_handle_t el,
                                 audio_port_handle_t port);
int audio_element_set_output_timeout(audio_element_handle_t el,
                                     TickType_t ticks);
int audio_element_set_input_timeout(audio_element_handle_t el,
                                    TickType_t ticks);
enum { AEL_STATE_INIT, AEL_STATE_ERROR };
int audio_element_get_state(audio_element_handle_t el);
int audio_element_run(audio_element_handle_t el);
int audio_element_resume(audio_element_handle_t el, float threshold,
                         TickType_t ticks);
int audio_element_stop(audio_element_handle_t el);
int audio_element_wait_for_stop_ms(audio_element_handle_t el, TickType_t ticks);
int audio_element_terminate_with_ticks(audio_element_handle_t el,
                                       TickType_t ticks);
int audio_element_deinit(audio_element_handle_t el);
#endif
#endif
