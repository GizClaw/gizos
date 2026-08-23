#include "h2_esp_board.h"
#include "h2_esp_h2loader_iostreamikcp.h"
#include "h2_esp_platform_core.h"
#include "h2_loader_app_client.h"
#include "h2_loader_boot.h"
#include "h2_smoke_wifi_csi.h"

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static h2_loader_app_client_t s_return_client;

static int
smoke_start_return_console(const h2_runtime_config_t *runtime_config) {
  int rc;
  h2_loader_app_client_config_t return_config = {
      .pref = runtime_config->pref,
      .power = runtime_config->power,
      .allocator = runtime_config->mem,
      .board = runtime_config->board,
      .target = runtime_config->target,
      .chip = runtime_config->chip,
      .active_name = "smoke-wifi-csi",
      .h2loader_partition_id = 1u,
  };
  rc = h2_loader_app_client_init(&s_return_client, &return_config);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  return h2_esp_h2loader_app_iostreamikcp_start(
      &s_return_client, runtime_config->task, runtime_config->mem, 8192u);
}

static void smoke_confirm_app(h2_runtime_t *runtime) {
  h2_pal_result_t confirm_rc = h2_esp_platform_confirm_running_app();
  if (confirm_rc != H2_PAL_OK) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=ota_confirm rc=%d\n", (int)confirm_rc);
    return;
  }
  int rc = h2_loader_mark_app_confirmed(runtime->pref);
  if (rc != H2_PAL_OK) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=h2loader_confirm rc=%d\n", rc);
  }
}

static void smoke_ready(void *user, int rc) {
  h2_runtime_t *runtime = user;
  if (rc != H2_DISPLAY_OK) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=initial_render rc=%d\n", rc);
    return;
  }
  printf("H2_SMOKE_WIFI_CSI_STAGE stage=dashboard_ready\n");
  smoke_confirm_app(runtime);
}

static void image_entry(void *user) {
  (void)user;
  h2_runtime_config_t board_config = {0};
  h2_runtime_t *runtime = NULL;
  int rc = h2_esp_board_runtime_config(&board_config);
  if (rc != H2_PAL_OK) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=runtime_config rc=%d\n", rc);
    return;
  }

  int command_rc = smoke_start_return_console(&board_config);
  if (command_rc != H2_PAL_OK) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=return_console rc=%d\n", command_rc);
  }

  rc = h2_runtime_init(&board_config, &runtime);
  if (rc != H2_PAL_OK) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=runtime_init rc=%d\n", rc);
    (void)h2_esp_board_runtime_deinit();
    return;
  }

  if (runtime->display == NULL) {
    printf("H2_SMOKE_WIFI_CSI_FAIL stage=display_init rc=%d\n",
           H2_PAL_ERR_UNAVAILABLE);
    h2_runtime_deinit(runtime);
    return;
  }

  h2_smoke_wifi_csi_config_t smoke_config = {
      .user = runtime,
      .ready = command_rc == H2_PAL_OK ? smoke_ready : NULL,
  };
  rc = h2_smoke_wifi_csi_run(runtime, &smoke_config);
  printf("H2_SMOKE_WIFI_CSI_EXIT rc=%d\n", rc);

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void app_main(void) {
  h2_pal_result_t rc =
      h2_esp_board_start_entry_task("szp/wifi-csi", image_entry, NULL);
  if (rc != H2_PAL_OK) {
    printf("H2_BOARD_ENTRY_FAIL board=szp image=wifi-csi code=%d\n", rc);
  }
}
