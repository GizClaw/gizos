#ifndef PAL_DISK_TEST_SDK_H
#define PAL_DISK_TEST_SDK_H
#include "h2_bk_uart_io_test_sdk.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define CONFIG_SDCARD_POWER_GPIO_CTRL 0
#define CONFIG_SDIO_4LINES_EN 1
#define GPIO_SDIO_MAP_MODE0 0
#define GPIO_OUTPUT_STATE_LOW 0
#define GPIO_OUTPUT_STATE_HIGH 1
typedef int gpio_output_state_e;
typedef int beken_thread_t;
void bk_init(void);
int rtos_get_core_id(void);
int rtos_core0_create_thread(beken_thread_t*, int, const char*, void (*)(void*),
                             unsigned, void*);
int h2_bk_target_task_policy_install(void);
int bk_sd_card_deinit(void);
int bk_sdio_host_driver_deinit(void);
int bk_sdio_host_driver_init(void);
int gpio_sdio_sel(int);
int gpio_sdio_one_line_sel(int);
#define DISK_NUMBER_SDIO_SD 1
int disk_uninitialize(int);
#define os_malloc malloc
#define os_free free
int os_printf(const char*, ...);
typedef unsigned char BYTE;
typedef unsigned UINT;
typedef uint32_t FSIZE_t;
typedef struct {
  int mounted;
} FATFS;
typedef struct {
  FSIZE_t pos;
} FIL;
typedef struct {
  int unused;
} DIR;
typedef struct {
  FSIZE_t fsize;
  unsigned fattrib;
  char fname[64];
} FILINFO;
typedef enum {
  FR_OK,
  FR_NO_FILE,
  FR_NO_PATH,
  FR_DENIED,
  FR_WRITE_PROTECTED,
  FR_NOT_ENOUGH_CORE,
  FR_TIMEOUT,
  FR_EXIST,
  FR_NOT_READY
} FRESULT;
#define FA_WRITE 1
#define FA_CREATE_ALWAYS 2
#define FA_READ 4
#define AM_DIR 16
FRESULT f_mount(FATFS*, const char*, int);
FRESULT f_mkdir(const char*);
FRESULT f_unlink(const char*);
FRESULT f_open(FIL*, const char*, BYTE);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_write(FIL*, const void*, UINT, UINT*);
FRESULT f_lseek(FIL*, FSIZE_t);
#define f_tell(f) ((f)->pos)
FRESULT f_sync(FIL*);
FRESULT f_close(FIL*);
FRESULT f_stat(const char*, FILINFO*);
FRESULT f_rename(const char*, const char*);
FRESULT f_opendir(DIR*, const char*);
FRESULT f_readdir(DIR*, FILINFO*);
FRESULT f_closedir(DIR*);
#endif
