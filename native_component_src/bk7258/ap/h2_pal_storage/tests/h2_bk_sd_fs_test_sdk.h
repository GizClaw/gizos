#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
typedef unsigned UINT;
typedef unsigned char BYTE;
typedef int FRESULT;
typedef int gpio_output_state_e;
typedef struct {
  int marker;
} FIL;
typedef struct {
  int marker;
} FATFS;
typedef struct {
  int marker;
} DIR;
typedef struct {
  unsigned fattrib;
  uint32_t fsize;
  char fname[64];
} FILINFO;
enum {
  FR_OK,
  FR_NO_FILE,
  FR_NO_PATH,
  FR_DENIED,
  FR_WRITE_PROTECTED,
  FR_NOT_ENOUGH_CORE,
  FR_TIMEOUT,
  FR_EXIST,
  FR_DISK_ERR
};
enum { FA_WRITE = 1, FA_CREATE_ALWAYS = 2, FA_READ = 4, AM_DIR = 0x10 };
enum {
  GPIO_OUTPUT_STATE_LOW,
  GPIO_OUTPUT_STATE_HIGH,
  GPIO_SDIO_MAP_MODE0,
  DISK_NUMBER_SDIO_SD
};
#define CONFIG_SDIO_4LINES_EN 1
#define CONFIG_SDCARD_POWER_GPIO_CTRL 0
#define os_malloc malloc
#define os_free free
#define os_printf(...) ((void)0)
int gpio_sdio_sel(int);
int gpio_sdio_one_line_sel(int);
int disk_uninitialize(int);
int bk_sd_card_deinit(void);
int bk_sdio_host_driver_deinit(void);
int bk_sdio_host_driver_init(void);
void rtos_delay_milliseconds(uint32_t);
FRESULT f_mount(FATFS*, const char*, int);
FRESULT f_mkdir(const char*);
FRESULT f_open(FIL*, const char*, BYTE);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_write(FIL*, const void*, UINT, UINT*);
FRESULT f_sync(FIL*);
FRESULT f_close(FIL*);
FRESULT f_stat(const char*, FILINFO*);
FRESULT f_unlink(const char*);
FRESULT f_rename(const char*, const char*);
FRESULT f_opendir(DIR*, const char*);
FRESULT f_readdir(DIR*, FILINFO*);
FRESULT f_closedir(DIR*);
