#ifndef H2_H202_MFG_LAUNCHER_PLATFORM_H
#define H2_H202_MFG_LAUNCHER_PLATFORM_H

/* Host substitutes only for native platform/SDK boundaries. The test includes
 * the production launcher and links the real acceptance gate and Loader Core.
 */
#define H2_BK_H2LOADER_APP_ENTRY_PATH "app/bk/app_ab_crc.rbl"
#define H2_BK_H2LOADER_PRIMARY_PARTITION_ID 1u
#define H2_BK_H2LOADER_APP_PARTITION_ID 2u
#define H2_BK_H2LOADER_COREDUMP_ADDR 0x7a0000u
#define H2_BK_H2LOADER_COREDUMP_SIZE (360u * 1024u)

h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void);
const h2_pal_task_api_t* h2_bk_platform_task_api(void);
const h2_pal_power_api_t* h2_bk_h2loader_power_api(void);
int h2_bk_h2loader_start_loader_iostreamikcp(
    h2_runtime_t* runtime, h2_loader_command_t* command,
    const h2_loader_command_config_t* config);
int h2_bk_h2loader_get_device_uid(char* out, size_t capacity);
int h2_bk_h2loader_confirm_active_loader(void* user);
int h2_bk_target_task_policy_install(void);
void bk_init(void);
void rtos_delay_milliseconds(uint32_t delay_ms);

static inline const h2_bundle_app_writer_t* h2_bk_h2loader_ota_app_writer(
    void) {
  return NULL;
}
static inline const h2_loader_image_reader_api_t* h2_bk_h2loader_image_reader(
    void) {
  return NULL;
}
static inline const h2_loader_image_writer_api_t* h2_bk_h2loader_image_writer(
    void) {
  return NULL;
}
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
