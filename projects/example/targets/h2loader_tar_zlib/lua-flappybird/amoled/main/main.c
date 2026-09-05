#include "board_config.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_lua_flappybird.h"

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>

static int never_stop(void *user) {
  (void)user;
  return 0;
}

static h2_pal_result_t confirm_ready(void *user) {
  h2_runtime_t *runtime = user;
  h2_pal_result_t result = h2_esp_platform_confirm_running_app();
  if (result == H2_PAL_OK) {
    result = h2_esp_h2loader_app_confirm(runtime);
  }
  if (result == H2_PAL_OK) {
    printf(
        "H2_LUA_FLAPPYBIRD_READY display=368x448 touch=ft3168 button=boot\n");
  }
  return result;
}

static void hold_for_recovery(void) {
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000u));
}

static void image_entry(void *user) {
  h2_runtime_config_t config = {0};
  h2_runtime_t *runtime = NULL;
  h2_pal_result_t result;
  (void)user;
  result = h2_lua_flappybird_amoled_runtime_config(&config);
  if (result == H2_PAL_OK) {
    result = h2_esp_h2loader_app_commands_prepare_serial(
        &config, "lua-flappybird", 1u, 3u);
  }
  if (result == H2_PAL_OK)
    result = h2_runtime_init(&config, &runtime);
  if (result == H2_PAL_OK) {
    result =
        h2_esp_h2loader_app_commands_start(runtime, "lua-flappybird", 1u, 3u);
  }
  if (result == H2_PAL_OK) {
    h2_runtime_input_poll_config_t input_poll = {0};
    result = h2_lua_flappybird_amoled_input_poll_config(&input_poll);
    if (result == H2_PAL_OK)
      result = h2_runtime_input_start(runtime, &input_poll);
  }
  if (result != H2_PAL_OK) {
    printf("H2_LUA_FLAPPYBIRD_FAIL stage=start rc=%d\n", (int)result);
    esp_restart();
  }
  result = h2_lua_flappybird_run(
      runtime, &(h2_lua_flappybird_config_t){
                   .button_component_id = H2_LUA_FLAPPYBIRD_COMPONENT_BACK,
                   .back_component_id = H2_RUNTIME_COMPONENT_ID_NONE,
                   .should_stop = never_stop,
                   .on_ready = confirm_ready,
                   .on_ready_user = runtime,
               });
  printf("H2_LUA_FLAPPYBIRD_EXIT rc=%d recovery=app_command\n", (int)result);
  hold_for_recovery();
}

void app_main(void) {
  h2_pal_result_t result =
      h2_esp_board_start_entry_task("amoled/lua-flappybird", image_entry, NULL);
  if (result != H2_PAL_OK) {
    printf("H2_BOARD_ENTRY_FAIL board=amoled image=lua-flappybird code=%d\n",
           (int)result);
  }
}
