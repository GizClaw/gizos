#ifndef H2_MOSAICO_AUDIO_TEST_SDK_H
#define H2_MOSAICO_AUDIO_TEST_SDK_H
/* Compile the actual runner and recovery code on the host. These startup-only
 * SDK shims are never executed by the failure-recovery oracle. */
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY (-1)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
static inline int h2_mosaico_loader_usb_init(void) { return 0; }
static inline int h2_esp_board_runtime_deinit(void) { return 0; }
static inline void vTaskDeleteWithCaps(void *handle) { (void)handle; }
static inline int
xTaskCreatePinnedToCoreWithCaps(void (*entry)(void *), const char *name,
                                unsigned stack, void *user, unsigned priority,
                                void *handle, int core, unsigned caps) {
  (void)entry;
  (void)name;
  (void)stack;
  (void)user;
  (void)priority;
  (void)handle;
  (void)core;
  (void)caps;
  return pdPASS;
}
#endif
