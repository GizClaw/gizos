#include "board_config.h"

#include "h2_esp_board.h"
#include "h2_esp_h2loader_ble.h"
#include "h2_esp_h2loader_runtime.h"
#include "h2_esp_platform_core.h"
#include "h2_esp_target_task_policy.h"
#include "h2_lua_qi_duel.h"

#include "esp_system.h"
#include "esp_rom_sys.h"
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
  if (result == H2_PAL_OK)
    result = h2_esp_h2loader_app_confirm(runtime);
  if (result == H2_PAL_OK) {
    printf("H2_LUA_QI_DUEL_READY display=368x448 touch=ft3168 "
           "button=boot target_fps=30\n");
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
  esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=entry\n");
  result = h2_lua_qi_duel_amoled_runtime_config(&config);
  esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=runtime_config rc=%d\n",
                 (int)result);
  if (result == H2_PAL_OK) {
    result = h2_esp_h2loader_app_commands_prepare_serial(
        &config, "lua-qi-duel", 1u, 3u);
    esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=prepare_commands rc=%d\n",
                   (int)result);
  }
  if (result == H2_PAL_OK) {
    result = h2_runtime_init(&config, &runtime);
    esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=runtime_init rc=%d\n",
                   (int)result);
  }
  if (result == H2_PAL_OK) {
    result =
        h2_esp_h2loader_app_commands_start(runtime, "lua-qi-duel", 1u, 3u);
    esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=commands_start rc=%d\n",
                   (int)result);
  }
  if (result == H2_PAL_OK) {
    h2_runtime_input_poll_config_t input_poll = {0};
    result = h2_lua_qi_duel_amoled_input_poll_config(&input_poll);
    if (result == H2_PAL_OK)
      result = h2_runtime_input_start(runtime, &input_poll);
    esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=input_start rc=%d\n",
                   (int)result);
  }
  if (result != H2_PAL_OK) {
    printf("H2_LUA_QI_DUEL_FAIL stage=start rc=%d\n", (int)result);
    esp_restart();
  }
  esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=lua_run\n");
  result = h2_lua_qi_duel_run(
      runtime, &(h2_lua_qi_duel_config_t){
                   .back_component_id = H2_LUA_QI_DUEL_COMPONENT_BACK,
                   .should_stop = never_stop,
                   .on_ready = confirm_ready,
                   .on_ready_user = runtime,
               });
  printf("H2_LUA_QI_DUEL_EXIT rc=%d recovery=app_command\n", (int)result);
  hold_for_recovery();
}

void app_main(void) {
  esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=app_main\n");
  h2_pal_result_t result = h2_esp_board_start_entry_task(
      H2_LUA_QI_DUEL_ENTRY_TASK_NAME_VALUE, image_entry, NULL);
  esp_rom_printf("H2_LUA_QI_DUEL_BOOT stage=entry_task rc=%d\n", (int)result);
  if (result != H2_PAL_OK) {
    printf("H2_BOARD_ENTRY_FAIL board=amoled image=lua-qi-duel code=%d\n",
           (int)result);
  }
}
