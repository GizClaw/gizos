#include "h2_bk7258_board.h"
#include "h2_bk_platform_storage.h"
#include "h2_h202_board_test_sdk.h"
#include "h2_h202_board_config.h"

#include <assert.h>
#include <string.h>

static int level = 1;
static unsigned format_requests, releases;
static const h2_bk_sd_fs_config_t* sd_config;

int bk_gpio_get_input(gpio_id_t pin) {
  assert(pin == H2_H202_SD_DETECT_GPIO);
  return level;
}
int gpio_sdio_sel(int mode) {
  assert(mode == GPIO_SDIO_MAP_MODE1);
  return H2_PAL_OK;
}
int h2_bk_platform_sd_fs_init(const h2_bk_sd_fs_config_t* config,
                              h2_pal_fs_api_t* fs) {
  assert(fs != NULL && config != NULL);
  sd_config = config;
  return H2_PAL_OK;
}
int h2_bk_platform_sd_fs_factory_mount(const char* path) {
  assert(!strcmp(path, H2_BK7258_DL_MOUNT_PATH));
  ++format_requests;
  return sd_config->card_present() ? H2_PAL_OK : H2_PAL_ERR_UNAVAILABLE;
}
int h2_bk_platform_sd_fs_mount(void* user, const char* path) {
  assert(user == NULL && !strcmp(path, H2_BK7258_DATA_MOUNT_PATH));
  return H2_PAL_OK;
}
void h2_bk_platform_sd_fs_release(void) { ++releases; }

int main(void) {
  h2_pal_fs_api_t fs;
  assert(h2_bk7258_board_fs_init(&fs) == H2_PAL_OK);
  assert(sd_config->select_pins() == H2_PAL_OK);
  assert(!strcmp(sd_config->drive, "1:") && sd_config->mount_count == 2u);
  assert(!strcmp(sd_config->mounts[0].native_path, "1:/h2loader/dl"));
  assert(!strcmp(sd_config->mounts[1].native_path, "1:/h2loader/data"));
  assert(!h2_bk7258_board_sd_present() && !format_requests);
  assert(h2_bk7258_board_sd_check() == H2_PAL_ERR_UNAVAILABLE);
  level = 0;
  assert(h2_bk7258_board_sd_present());
  assert(h2_bk7258_board_sd_check() == H2_PAL_OK && format_requests == 2u);
  assert(h2_bk7258_board_fs_deinit() == H2_PAL_OK && releases == 1u);
  return 0;
}
