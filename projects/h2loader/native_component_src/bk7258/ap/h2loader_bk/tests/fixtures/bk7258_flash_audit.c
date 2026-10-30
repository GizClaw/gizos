/* Test-image-only link wrappers. Never link into production firmware.
 * Wrap bk_flash_erase_sector, bk_flash_write_bytes,
 * h2_bk_h2loader_image_writer and h2_loader_command_init. */
#include "h2_bk_h2loader.h"
#include "h2_loader_command.h"
#include "driver/flash.h"
#include <stdio.h>
#include <string.h>

extern unsigned h2_bk_flash_aes_state(void);
extern bk_err_t __real_bk_flash_erase_sector(uint32_t address);
extern bk_err_t __real_bk_flash_write_bytes(uint32_t address,
                                            const uint8_t* data, uint32_t size);
extern const h2_loader_image_writer_api_t* __real_h2_bk_h2loader_image_writer(
    void);
extern int __real_h2_loader_command_init(
    h2_loader_command_t* command, const h2_loader_command_config_t* config);
static unsigned s_erases, s_writes, s_observing, s_valid, s_mode;
static int s_begin_rc;
static h2_loader_image_writer_api_t s_api;
static h2_loader_image_writer_vtable_t s_vtable;
static const h2_loader_image_writer_api_t* s_real;
static h2_command_handler_fn s_stats;

bk_err_t __wrap_bk_flash_erase_sector(uint32_t address) {
  if (s_observing) ++s_erases;
  return __real_bk_flash_erase_sector(address);
}
bk_err_t __wrap_bk_flash_write_bytes(uint32_t address, const uint8_t* data,
                                     uint32_t size) {
  if (s_observing) ++s_writes;
  return __real_bk_flash_write_bytes(address, data, size);
}
static int audit_begin(void* user, uint32_t partition,
                       const h2_loader_image_identity_t* identity) {
  if (identity && identity->role == H2_LOADER_IMAGE_ROLE_H2LOADER) {
    s_valid = 0;
    s_erases = 0;
    s_writes = 0;
    s_mode = h2_bk_flash_aes_state();
    s_observing = 1;
    s_begin_rc = s_real->vtable->begin(user, partition, identity);
    s_observing = 0;
    s_valid = 1;
    return s_begin_rc;
  }
  return s_real->vtable->begin(user, partition, identity);
}
const h2_loader_image_writer_api_t* __wrap_h2_bk_h2loader_image_writer(void) {
  s_real = __real_h2_bk_h2loader_image_writer();
  s_api = *s_real;
  s_vtable = *s_real->vtable;
  s_vtable.begin = audit_begin;
  s_api.vtable = &s_vtable;
  return &s_api;
}
static h2_pal_result_t audit_stats(void* user, h2_command_t* command,
                                   size_t argc, const char* const* argv) {
  char line[180];
  int n = snprintf(
      line, sizeof(line),
      "H2_BK_FLASH_REJECTION valid=%u aes=%u rc=%d erase_calls=%u write_calls=%u\n",
      s_valid, s_mode, s_begin_rc, s_erases, s_writes);
  if (n < 0 || (size_t)n >= sizeof(line)) return H2_PAL_ERR_NO_SPACE;
  h2_pal_result_t rc = h2_command_write(command, line, (size_t)n);
  return rc == H2_PAL_OK ? s_stats(user, command, argc, argv) : rc;
}
int __wrap_h2_loader_command_init(h2_loader_command_t* command,
                                  const h2_loader_command_config_t* config) {
  int rc = __real_h2_loader_command_init(command, config);
  if (rc != H2_PAL_OK) return rc;
  for (size_t i = 0; i < H2_LOADER_COMMAND_DEFINITION_CAPACITY; ++i) {
    if (command->definitions[i].path &&
        strcmp(command->definitions[i].path, "h2loader stats") == 0) {
      s_stats = command->definitions[i].handler;
      command->definitions[i].handler = audit_stats;
    }
  }
  return rc;
}
