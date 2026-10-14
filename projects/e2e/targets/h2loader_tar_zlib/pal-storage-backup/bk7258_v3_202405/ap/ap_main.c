#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_pal_e2e_application_host.h"
#include <driver/flash.h>
#include <driver/flash_partition.h>
#include <mbedtls/sha256.h>
#include <os/os.h>
#include <stdint.h>
#include <stdio.h>

static void fail(const char *stage, int rc) {
  printf("H2_PREF_BACKUP_FAIL stage=%s rc=%d\r\n", stage, rc);
  fflush(stdout);
  h2_pal_e2e_application_host_park();
}

/* Read before opening Preferences: this captures the old DB and the untouched
 * reserved tail before any new database is allowed to initialize there. */
static void dump_region(void) {
  const bk_logic_partition_t *partition =
      bk_flash_partition_get_info(BK_PARTITION_FLASHDB);
  if (partition == NULL || partition->partition_start_addr != 0x780000u ||
      partition->partition_length != 0x20000u)
    fail("partition", -1);
  printf("H2_PREF_BACKUP_BEGIN address=%u bytes=%u chunk_bytes=64\r\n",
         (unsigned)partition->partition_start_addr,
         (unsigned)partition->partition_length);
  fflush(stdout);
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts(&sha, 0) != 0)
    fail("sha-start", -1);
  const char digits[] = "0123456789abcdef";
  for (uint32_t offset = 0u; offset < partition->partition_length;
       offset += 64u) {
    uint8_t bytes[64];
    int rc = bk_flash_read_bytes(partition->partition_start_addr + offset,
                                 bytes, sizeof(bytes));
    if (rc != BK_OK)
      fail("read", rc);
    if (mbedtls_sha256_update(&sha, bytes, sizeof(bytes)) != 0)
      fail("sha-update", -1);
    char line[192];
    int count =
        snprintf(line, sizeof(line),
                 "H2_PREF_BACKUP_CHUNK offset=%u hex=", (unsigned)offset);
    if (count < 0 || (size_t)count + 128u >= sizeof(line))
      fail("format", -1);
    for (size_t i = 0u; i < sizeof(bytes); ++i) {
      line[count + i * 2u] = digits[bytes[i] >> 4u];
      line[count + i * 2u + 1u] = digits[bytes[i] & 15u];
    }
    line[count + 128] = '\0';
    printf("%s\r\n", line);
    fflush(stdout);
    rtos_delay_milliseconds(1u);
  }
  uint8_t digest[32];
  if (mbedtls_sha256_finish(&sha, digest) != 0)
    fail("sha-finish", -1);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (size_t i = 0u; i < sizeof(digest); ++i) {
    hex[i * 2u] = digits[digest[i] >> 4u];
    hex[i * 2u + 1u] = digits[digest[i] & 15u];
  }
  hex[64] = '\0';
  printf("H2_PREF_BACKUP_END bytes=%u sha256=%s\r\n",
         (unsigned)partition->partition_length, hex);
  fflush(stdout);
}

static h2_pal_result_t before_prepare(void *user) {
  (void)user;
  puts("H2_PREF_BACKUP_PLATFORM_BOOT board=bk7258");
  fflush(stdout);
  dump_region();
  return H2_PAL_OK;
}

static h2_pal_result_t run(h2_application_host_t *host,
                           h2_runtime_t *runtime, void *user) {
  (void)user;
  int rc = H2_PAL_OK;
  rc = h2_bk_h2loader_confirm_current_app(runtime);
  printf("H2_PREF_BACKUP_READY confirm=%d\r\n", rc);
  fflush(stdout);
  h2_pal_result_t waited = h2_pal_e2e_application_host_wait(host, runtime);
  return rc == H2_PAL_OK ? waited : rc;
}

int main(void) {
    const h2_application_host_config_t app = {
        .entry = run, .before_prepare = before_prepare};
    return h2_pal_e2e_application_host_start(
        "pref-backup", H2_LOADER_CAPABILITY_UART | H2_LOADER_CAPABILITY_WIFI, &app);
}
