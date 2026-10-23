#include "h2_lingxi.h"
#include "h2_lingxi_e2e.h"

#include <string.h>

int main(void) {
  h2_lingxi_device_info_t info;
  memset(&info, 0xa5, sizeof(info));
  h2_lingxi_device_info_clear(&info);
  if (info.device_secret[0] != '\0') return 1;
  h2_lingxi_result_t result;
  if (h2_lingxi_get_device_info(NULL, NULL, NULL, &info, &result) !=
      H2_PAL_ERR_INVALID_ARG)
    return 2;
  h2_lingxi_e2e_result_t ledger;
  return h2_lingxi_e2e_run(NULL, &ledger) == H2_PAL_ERR_INVALID_ARG ? 0 : 3;
}
