#ifndef H2_H202_MFG_LAUNCHER_PLATFORM_H
#define H2_H202_MFG_LAUNCHER_PLATFORM_H

/* Host substitutes only for native platform/SDK boundaries. The test includes
 * the production launcher and links the real acceptance gate and Loader Core.
 */

h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void);
const h2_pal_task_api_t* h2_bk_platform_task_api(void);
const h2_pal_power_api_t* h2_bk_platform_power_api(void);
int h2_h202_loader_uart_start(h2_runtime_t* runtime,
                              h2_loader_command_t* command,
                              const h2_loader_command_config_t* config);
int h2_bk_target_task_policy_install(void);
void bk_init(void);
void rtos_delay_milliseconds(uint32_t delay_ms);

static inline uint32_t rtos_get_time(void) { return 0u; }
static inline int bk_wdt_feed(void) { return 0; }

typedef int flash_protect_type_t;
#define FLASH_PROTECT_NONE 0
static inline int bk_flash_get_protect_type(void) { return 0; }
static inline int bk_flash_set_protect_type(flash_protect_type_t type) {
  (void)type;
  return 0;
}
static inline int bk_flash_erase_sector(uint32_t address) {
  (void)address;
  return 0;
}
static inline int bk_flash_read_bytes(uint32_t address, void* data,
                                      uint32_t len) {
  (void)address;
  (void)data;
  (void)len;
  return -1;
}

typedef struct mbedtls_sha256_context {
  int unused;
} mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context* ctx) {
  (void)ctx;
}
static inline void mbedtls_sha256_free(mbedtls_sha256_context* ctx) {
  (void)ctx;
}
static inline int mbedtls_sha256_starts(mbedtls_sha256_context* ctx,
                                        int is224) {
  (void)ctx;
  (void)is224;
  return 0;
}
static inline int mbedtls_sha256_update(mbedtls_sha256_context* ctx,
                                        const uint8_t* data, size_t len) {
  (void)ctx;
  (void)data;
  (void)len;
  return 0;
}
static inline int mbedtls_sha256_finish(mbedtls_sha256_context* ctx,
                                        uint8_t out[32]) {
  (void)ctx;
  (void)out;
  return -1;
}

#endif
