#include "h2_esp_alipay_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "h2_esp_alipay_socket_abi.h"

void alipay_iot_gettimeofday(alipay_iot_timeval* out) {
  if (out == NULL) return;
  uint64_t ms = 0u;
  if (h2_pal_time_get_wall_ms(h2_esp_alipay_config()->time, &ms) != H2_PAL_OK)
    ms = 0u;
  out->tv_sec = (long)(ms / 1000u);
  out->tv_usec = (long)(ms % 1000u) * 1000;
}
void alipay_task_sleep(uint32_t ms) {
  (void)h2_pal_time_sleep_ms(h2_esp_alipay_config()->time, ms);
}
alipay_iot_mutex alipay_iot_mutex_create(const char* name) {
  (void)name;
  return xSemaphoreCreateMutex();
}
int alipay_iot_mutex_lock(alipay_iot_mutex mutex) {
  return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE ? 0
                                                                         : -1;
}
int alipay_iot_mutex_unlock(alipay_iot_mutex mutex) {
  return mutex != NULL && xSemaphoreGive(mutex) == pdTRUE ? 0 : -1;
}
int alipay_iot_mutex_delete(alipay_iot_mutex mutex) {
  if (mutex == NULL) return -1;
  vSemaphoreDelete(mutex);
  return 0;
}
