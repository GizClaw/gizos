#include "h2_bk7258_board.h"
#include "h2_bk_platform_storage.h"
#include "h2_h202_board_config.h"

#ifdef H2_H202_HOST_TEST
#include "h2_h202_board_test_sdk.h"
#else
#include "driver/gpio.h"
#include "gpio_driver.h"
#endif

static const h2_bk_sd_mount_t s_mounts[] = {
    {H2_BK7258_DL_MOUNT_PATH, "1:/h2loader/dl"},
    {H2_BK7258_DATA_MOUNT_PATH, "1:/h2loader/data"},
};

static int select_sd_pins(void) {
  return gpio_sdio_sel(GPIO_SDIO_MAP_MODE1) == 0 ? H2_PAL_OK : H2_PAL_ERR_IO;
}

bool h2_bk7258_board_sd_present(void) {
  return bk_gpio_get_input((gpio_id_t)H2_H202_SD_DETECT_GPIO) ==
         H2_H202_SD_DETECT_ACTIVE_LEVEL;
}

int h2_bk7258_board_fs_init(h2_pal_fs_api_t* fs) {
  static const h2_bk_sd_fs_config_t config = {
      .drive = "1:",
      .root = "1:/h2loader",
      .mounts = s_mounts,
      .mount_count = sizeof(s_mounts) / sizeof(s_mounts[0]),
      .select_pins = select_sd_pins,
      .card_present = h2_bk7258_board_sd_present,
  };
  return h2_bk_platform_sd_fs_init(&config, fs);
}

int h2_bk7258_board_sd_check(void) {
  int rc = h2_bk_platform_sd_fs_factory_mount(H2_BK7258_DL_MOUNT_PATH);
  if (rc == H2_PAL_OK)
    rc = h2_bk_platform_sd_fs_mount(NULL, H2_BK7258_DATA_MOUNT_PATH);
  return rc;
}

int h2_bk7258_board_fs_deinit(void) {
  h2_bk_platform_sd_fs_release();
  return H2_PAL_OK;
}
