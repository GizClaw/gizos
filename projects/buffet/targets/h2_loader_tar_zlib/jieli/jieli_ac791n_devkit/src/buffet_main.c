/* JieLi AC791N DevKit App image: Runtime lifetime, H2Loader App commands over
 * the USB console, and the game task. The game confirms the trial boot once
 * its first frame is visible with Touch open. Until then a timeout resets the
 * device so that the unconfirmed trial returns to Loader. */
#include "app_config.h"

#include "asm/cpu.h"
#include "device/device.h"
#include "os/os_api.h"
#include "system/task.h"
#include "system/timer.h"

#include "h2_atomic.h"
#include "h2_device_service.h"
#include "h2_buffet.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_wl82_console.h"
#include "buffet_frame_profile.h"

#include <stdio.h>

#define CONFIRM_TIMEOUT_MS 120000u

static h2_device_app_commands_t h2_management;
static h2_runtime_t* runtime;
static h2_atomic_uint_t confirmed;
static int commands_ready;

/* The shipping Loader already copies marker.result before boot probes start
 * again. Publish one word from the game task, including during a total stall.
 */
#define CHECKPOINT_TAG UINT32_C(0x50000000)
#define CHECKPOINT_DINING UINT32_C(0x40)
_Static_assert(H2_BUFFET_STAGE_CLEANUP < 32, "checkpoint stage must fit");
static uint32_t late_frames, completed_rests;
static buffet_frame_profile_t frame_profile;

void h2_jieli_ac791n_devkit_display_trace(h2_jieli_display_stage_t stage) {
  if (frame_profile.active) {
    static const enum profile_part parts[] = {PF_LCD_DRAIN, PF_LCD_WINDOW,
                                              PF_LCD_PACK, PF_LCD_SUBMIT};
    profile_change(&frame_profile, runtime->time, parts[stage]);
  }
}

static void report_profile(void* user) {
  (void)user;
  printf(
      "H2_BUFFET_PROFILE n=%u failed=%u sum_us game=%u touch=%u audio=%u "
      "mix=%u write=%u copy=%u scene=%u diff=%u lcd_other=%u drain=%u "
      "window=%u pack=%u submit=%u wait=%u rest=%u other=%u\r\n",
      frame_profile.samples, frame_profile.failed, frame_profile.total[PF_GAME],
      frame_profile.total[PF_TOUCH], frame_profile.total[PF_AUDIO],
      frame_profile.total[PF_MIX], frame_profile.total[PF_WRITE],
      frame_profile.total[PF_COPY], frame_profile.total[PF_SCENE],
      frame_profile.total[PF_DIFF], frame_profile.total[PF_LCD_OTHER],
      frame_profile.total[PF_LCD_DRAIN], frame_profile.total[PF_LCD_WINDOW],
      frame_profile.total[PF_LCD_PACK], frame_profile.total[PF_LCD_SUBMIT],
      frame_profile.total[PF_WAIT], frame_profile.total[PF_REST],
      frame_profile.total[PF_OTHER]);
  frame_profile.samples = frame_profile.failed = 0;
  for (unsigned i = 0; i < PF_PARTS; ++i) frame_profile.total[i] = 0;
}

static void checkpoint(void* user, h2_buffet_stage_t stage) {
  (void)user;
  static uint32_t frame, dining;
  profile_stage(&frame_profile, runtime->time, stage);
  if (stage == H2_BUFFET_STAGE_FRAME || stage == H2_BUFFET_STAGE_DINING_FRAME) {
    ++frame;
    dining = stage == H2_BUFFET_STAGE_DINING_FRAME ? CHECKPOINT_DINING : 0u;
  }
  if (stage == H2_BUFFET_STAGE_LATE)
    __atomic_fetch_add(&late_frames, 1u, __ATOMIC_RELAXED);
  if (stage == H2_BUFFET_STAGE_REST_DONE)
    __atomic_fetch_add(&completed_rests, 1u, __ATOMIC_RELAXED);
  h2_jieli_wl82_app_breadcrumb_set(CHECKPOINT_TAG | ((frame & 0xfffffu) << 8) |
                                   dining | ((uint32_t)current_cpu_id() << 5) |
                                   (uint32_t)stage);
}

/* SDK lifetime idle tasks. This SDK resets their short profiling counters on
 * vTaskGetInfo. Request only the interval count, without scanning the stacks.
 */
static int idle_runtime(unsigned core) {
  static TaskHandle_t idle[2];
  if (idle[core] == NULL) idle[core] = xTaskGetHandle(core ? "IDLE1" : "IDLE0");
  if (idle[core] == NULL) return -1;
  TaskStatus_t status = {0};
  vTaskGetInfo(idle[core], &status, 0, eReady);
  return (int)status.ulRunTimeCounter;
}

/* Existing low-priority UART log worker, about once a second. Reading each
 * core's trace/register window does not halt either CPU. Asynchronous samples
 * may span a switch or include this observer. The retained game word also
 * survives when this worker can no longer run. */
void h2_jieli_usb_debug_heartbeat(void) {
  uint32_t word = (uint32_t)h2_jieli_wl82_app_breadcrumb_get();
  if ((word & (UINT32_C(0xf0000000) | CHECKPOINT_DINING)) !=
      (CHECKPOINT_TAG | CHECKPOINT_DINING))
    return;
  uint64_t start = 0, end = 0;
  bool timed = h2_pal_time_get_monotonic_us(runtime->time, &start) == H2_PAL_OK;
  static uint32_t previous_diag_us;
  unsigned pending = h2_jieli_ac791n_devkit_watchdog_pending();
  unsigned trace0 = q32DSP(0)->ETM_PC0, lr0 = q32DSP(0)->RETS,
           sp0 = q32DSP(0)->SP;
  unsigned trace1 = q32DSP(1)->ETM_PC0, lr1 = q32DSP(1)->RETS,
           sp1 = q32DSP(1)->SP;
  printf(
      "H2_BUFFET_SCHED mark=%08x late=%u rested=%u pending=%u idle=%d,%d "
      "c0=%x/%x/%x c1=%x/%x/%x previous_diag_us=%u\r\n",
      (unsigned)word, (unsigned)__atomic_load_n(&late_frames, __ATOMIC_RELAXED),
      (unsigned)__atomic_load_n(&completed_rests, __ATOMIC_RELAXED), pending,
      idle_runtime(0), idle_runtime(1), trace0, lr0, sp0, trace1, lr1, sp1,
      previous_diag_us);
  if (timed && h2_pal_time_get_monotonic_us(runtime->time, &end) == H2_PAL_OK &&
      end >= start)
    previous_diag_us = (uint32_t)(end - start);
}

static void return_to_loader(void* user) {
  (void)user;
  if (h2_atomic_load_explicit(&confirmed, H2_ATOMIC_ACQUIRE) != 0u) return;
  printf("H2_BUFFET_CONFIRM_TIMEOUT\r\n");
  system_reset();
}

/* Runs before app_main; the SDK calls this weak hook when it is defined. */
int h2_jieli_ac791n_devkit_early_app_boot(void) {
  uint16_t timer = sys_timeout_add_to_task("sys_timer", NULL, return_to_loader,
                                           CONFIRM_TIMEOUT_MS);
  if (timer == 0u) {
    system_reset();
    return -1;
  }
  return 0;
}

static void hold(void) {
  for (;;) os_time_dly(100u);
}

static h2_pal_result_t confirm(void* user) {
  (void)user;
  (void)h2_pal_display_set_brightness_percent(runtime->display, 100);
  const int rc = h2_device_app_confirm(&h2_management, runtime);
  if (rc == H2_PAL_OK)
    h2_atomic_store_explicit(&confirmed, 1u, H2_ATOMIC_RELEASE);
  printf("H2_BUFFET_CONFIRM rc=%d\r\n", rc);
  return rc;
}

static void game(void* user) {
  (void)user;
  const h2_buffet_config_t config = {.ready = confirm,
                                     .cooperate_on_overrun = true,
                                     .checkpoint = checkpoint,
                                     .report_profile = report_profile};
  const int rc = h2_buffet_run(runtime, &config);
  /* The run only ends on failure; keep App commands up for recovery. An
   * unconfirmed run still returns to Loader through the boot timeout. */
  printf("H2_BUFFET_SETUP_FAIL stage=game rc=%d\r\n", rc);
  hold();
}

static int start(void) {
  /* Runtime FS needs the SD card the SDK mounts asynchronously. */
  for (unsigned i = 0; !dev_online("sd0") && i < 500u; ++i) os_time_dly(1u);
  /* The stick and bowl can each own a contact. Select IDs before Touch opens.
   */
  int rc = h2_jieli_ac791n_devkit_enable_multi_touch();
  if (rc != H2_PAL_OK) return rc;
  h2_runtime_config_t runtime_config;
  rc = h2_jieli_ac791n_devkit_runtime_config(&runtime_config);
  if (rc != H2_PAL_OK) return rc;
  rc = h2_runtime_init(&runtime_config, &runtime);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_app_commands_config_t commands = {
      .active_name = "tidewheel-sushi",
      .h2loader_partition_id = H2_JIELI_PARTITION_LOADER,
      .app_partition_id = H2_JIELI_PARTITION_APP,
      .coredump_partition_id = H2_JIELI_PARTITION_COREDUMP,
      .hardware_capabilities = H2_LOADER_CAPABILITY_UART,
      .app_entry_path = "app/jieli/update.ufw",
      .boot_policy_flags =
          H2_DEVICE_BOOT_TRACK_TRIAL | H2_DEVICE_BOOT_PERSIST_RECOVERY |
          H2_DEVICE_BOOT_STORED_SHADOW | H2_DEVICE_BOOT_PREPARE_SELECTION,
      .identity_policy = H2_DEVICE_IDENTITY_COMMITTED,
      .serial_admission_fallback_ms = H2_DEVICE_APP_ADMISSION_FALLBACK_MS,
      .command_stack_size = 48u * 1024u,
  };
  rc = h2_device_app_commands_prepare_runtime(&h2_management, runtime,
                                              &commands);
  if (rc == H2_PAL_OK)
    rc = h2_device_app_commands_start_serial(&h2_management, NULL);
  if (rc != H2_PAL_OK) return rc;
  commands_ready = 1;
  printf(
      "H2_BUFFET_BOOT board=jieli_ac791n_devkit render=480x288 lcd=480x320 frame_storage=967680\r\n");
  const h2_pal_task_options_t options = {.name = "tidewheel-sushi/game",
                                         .min_stack_size = 32768u};
  h2_pal_task_t* task = NULL;
  return h2_pal_task_start(runtime->task, &options, game, NULL, &task);
}

/* Return to the SDK app_core event loop once the game task runs. */
void app_main(void) {
  if (h2_atomic_init(&confirmed, 0u) != H2_ATOMIC_OK) {
    system_reset();
    return;
  }
  const int rc = start();
  if (rc == H2_PAL_OK) return;
  printf("H2_BUFFET_SETUP_FAIL stage=start rc=%d commands=%d\r\n", rc,
         commands_ready);
  /* Without App commands nothing can recover the device: reset so the
   * unconfirmed trial returns to Loader. Otherwise the boot timeout does. */
  if (!commands_ready) system_reset();
}
