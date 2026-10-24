#ifndef H2_MOSAICO_USB_TEST_SDK_H
#define H2_MOSAICO_USB_TEST_SDK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef _MSC_VER
#include <fcntl.h>
typedef intptr_t ssize_t;
#define O_ACCMODE (_O_RDONLY | _O_WRONLY | _O_RDWR)
#endif
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NOT_FINISHED, ESP_ERR_TIMEOUT };
typedef int TickType_t;
typedef struct {
  int held;
} StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {
  int unused;
} tinyusb_config_t;
#define TINYUSB_DEFAULT_CONFIG() {0}
typedef struct {
  bool dtr;
} h2_test_line_state_t;
typedef struct {
  h2_test_line_state_t line_state_changed_data;
} cdcacm_event_t;
typedef struct {
  int cdc_port;
  void (*callback_line_state_changed)(int, cdcacm_event_t*);
} tinyusb_config_cdcacm_t;
#define TINYUSB_CDC_ACM_0 0
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t*);
int xSemaphoreTake(SemaphoreHandle_t, TickType_t);
void xSemaphoreGive(SemaphoreHandle_t);
void vTaskDelay(TickType_t);
int64_t esp_timer_get_time(void);
int tinyusb_driver_install(const tinyusb_config_t*);
int tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t*);
bool tud_cdc_n_connected(int);
bool tud_cdc_n_write_clear(int);
void tud_cdc_n_read_flush(int);
size_t tinyusb_cdcacm_write_queue(int, const void*, size_t);
int tinyusb_cdcacm_write_flush(int, TickType_t);
int tinyusb_cdcacm_read(int, void*, size_t, size_t*);
typedef struct {
  int flags;
  int (*open)(const char*, int, int);
  int (*close)(int);
  ssize_t (*write)(int, const void*, size_t);
} esp_vfs_t;
#define ESP_VFS_FLAG_DEFAULT 0
int esp_vfs_register(const char*, const esp_vfs_t*, void*);
FILE* h2_test_freopen(const char*, const char*, FILE*);
#define freopen h2_test_freopen
#endif
