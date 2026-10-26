#include "h2_h2loader_host_lifecycle.h"
#include <assert.h>
#include <stddef.h>
int main(void) {
  h2_h2loader_host_status_t before = {0};
  h2_h2loader_host_status_t after = {0};
  assert(h2_h2loader_host_verify_reboot_status(
             H2_H2LOADER_HOST_COMMAND_REBOOT_APP, NULL, &after) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_h2loader_host_verify_reboot_status(
             H2_H2LOADER_HOST_COMMAND_REBOOT_APP, &before, NULL) ==
         H2_PAL_ERR_INVALID_ARG);
  after.active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_APP;
  after.running_partition = 2;
  after.next_partition = 2;
  after.boot_intent = H2_H2LOADER_HOST_BOOT_INTENT_AUTO;
  after.last = H2_PAL_ERR_IO;
  assert(h2_h2loader_host_verify_reboot_status(
             H2_H2LOADER_HOST_COMMAND_REBOOT_APP, &before, &after) ==
         H2_PAL_OK);
  after.running_partition = 1;
  assert(h2_h2loader_host_verify_reboot_status(
             H2_H2LOADER_HOST_COMMAND_REBOOT_APP, &before, &after) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_h2loader_host_verify_reboot_status(H2_H2LOADER_HOST_COMMAND_STATUS,
                                               &before, &after) ==
         H2_PAL_ERR_INVALID_STATE);
  return 0;
}
