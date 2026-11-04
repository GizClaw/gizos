#include "h2_h202_mfg.h"

#include "h2_loader_boot.h"
#include "h2_ntp.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- Controller ------------------------------------------------------- */

static const char* const s_item_names[H2_H202_MFG_ITEM_COUNT] = {
    "charger", "nfc",   "wifi", "left_mic", "right_mic",
    "listen",  "power", "plus", "minus",    "aging"};

static const uint8_t s_stages[H2_H202_MFG_ITEM_COUNT] = {2, 3, 4, 5, 5,
                                                         5, 6, 7, 8, 9};

static const h2_h202_mfg_color_t s_colors[H2_H202_MFG_ITEM_COUNT] = {
    H2_H202_MFG_COLOR_AMBER,      H2_H202_MFG_COLOR_PURPLE,
    H2_H202_MFG_COLOR_CYAN,       H2_H202_MFG_COLOR_BLUE_LEFT,
    H2_H202_MFG_COLOR_BLUE_RIGHT, H2_H202_MFG_COLOR_BLUE,
    H2_H202_MFG_COLOR_BLUE,       H2_H202_MFG_COLOR_BLUE,
    H2_H202_MFG_COLOR_BLUE,       H2_H202_MFG_COLOR_OFF};

static const h2_h202_mfg_prompt_t s_instructions[H2_H202_MFG_ITEM_COUNT] = {
    H2_H202_MFG_PROMPT_NONE,      H2_H202_MFG_PROMPT_NFC,
    H2_H202_MFG_PROMPT_NONE,      H2_H202_MFG_PROMPT_MIC_LEFT,
    H2_H202_MFG_PROMPT_MIC_RIGHT, H2_H202_MFG_PROMPT_LISTEN,
    H2_H202_MFG_PROMPT_POWER,     H2_H202_MFG_PROMPT_PLUS,
    H2_H202_MFG_PROMPT_MINUS,     H2_H202_MFG_PROMPT_AGING};

static const h2_h202_mfg_prompt_t s_successes[H2_H202_MFG_ITEM_COUNT] = {
    H2_H202_MFG_PROMPT_CHARGE_PASSED, H2_H202_MFG_PROMPT_NFC_PASSED,
    H2_H202_MFG_PROMPT_WIFI_PASSED,   H2_H202_MFG_PROMPT_NONE,
    H2_H202_MFG_PROMPT_NONE,          H2_H202_MFG_PROMPT_NONE,
    H2_H202_MFG_PROMPT_NONE,          H2_H202_MFG_PROMPT_NONE,
    H2_H202_MFG_PROMPT_NONE,          H2_H202_MFG_PROMPT_NONE};

/* Key items cannot fail on their own; only an instruction prompt error can
 * fail them, and that error is announced by the red light alone. */
static const h2_h202_mfg_prompt_t s_failures[H2_H202_MFG_ITEM_COUNT] = {
    H2_H202_MFG_PROMPT_CHARGE_FAILED, H2_H202_MFG_PROMPT_NFC_FAILED,
    H2_H202_MFG_PROMPT_WIFI_FAILED,   H2_H202_MFG_PROMPT_MIC_FAILED,
    H2_H202_MFG_PROMPT_MIC_FAILED,    H2_H202_MFG_PROMPT_NONE,
    H2_H202_MFG_PROMPT_NONE,          H2_H202_MFG_PROMPT_NONE,
    H2_H202_MFG_PROMPT_NONE,          H2_H202_MFG_PROMPT_AGING_FAILED};

unsigned h2_h202_mfg_stage_of(h2_h202_mfg_item_t item) {
  return (unsigned)item < H2_H202_MFG_ITEM_COUNT ? s_stages[item] : 10u;
}

static void note(h2_h202_mfg_t* app, const char* format, ...) {
  if (app->config.callbacks.log == NULL) return;
  char line[160];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  app->config.callbacks.log(app->config.callbacks.user, line);
}

static const char* item_name(h2_h202_mfg_item_t item) {
  return (unsigned)item < H2_H202_MFG_ITEM_COUNT ? s_item_names[item] : "wait";
}

static uint32_t new_generation(h2_h202_mfg_t* app) {
  if (++app->next_generation == 0u) ++app->next_generation;
  return app->next_generation;
}

/* Discard: completions of the canceled generation become stale. */
static void stop(h2_h202_mfg_t* app, h2_h202_mfg_operation_t operation) {
  uint32_t generation = app->current[operation];
  if (generation == 0u) return;
  app->current[operation] = 0u;
  app->config.callbacks.cancel(app->config.callbacks.user, operation,
                               generation);
}

static h2_pal_result_t begin(h2_h202_mfg_t* app,
                             h2_h202_mfg_operation_t operation,
                             h2_h202_mfg_job_t job, h2_h202_mfg_item_t item,
                             h2_h202_mfg_prompt_t first,
                             h2_h202_mfg_prompt_t second) {
  stop(app, operation);
  h2_h202_mfg_request_t request = {.operation = operation,
                                   .generation = new_generation(app),
                                   .job = job,
                                   .item = item,
                                   .prompts = {first, second}};
  request.prompt_count = first == H2_H202_MFG_PROMPT_NONE    ? 0u
                         : second == H2_H202_MFG_PROMPT_NONE ? 1u
                                                             : 2u;
  if (job == H2_H202_MFG_JOB_SHUTDOWN || job == H2_H202_MFG_JOB_ACCEPT)
    memcpy(request.results, app->results, sizeof(request.results));
  if (operation == H2_H202_MFG_OP_AUDIO) {
    app->audio_job = job;
    if (first != H2_H202_MFG_PROMPT_NONE) app->prompt = first;
  }
  h2_pal_result_t rc =
      app->config.callbacks.begin(app->config.callbacks.user, &request);
  if (rc == H2_PAL_OK) app->current[operation] = request.generation;
  return rc;
}

static h2_pal_result_t play(h2_h202_mfg_t* app, h2_h202_mfg_prompt_t prompt) {
  return begin(app, H2_H202_MFG_OP_AUDIO, H2_H202_MFG_JOB_PROMPT, app->item,
               prompt, H2_H202_MFG_PROMPT_NONE);
}

static void light(h2_h202_mfg_t* app, h2_h202_mfg_color_t color) {
  app->color = color;
  h2_pal_result_t rc =
      app->config.callbacks.light(app->config.callbacks.user, color);
  if (rc != H2_PAL_OK) note(app, "H202_MFG_LIGHT color=%d rc=%d", color, rc);
}

static bool all_passed(const h2_h202_mfg_t* app, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    if (app->results[i] != H2_H202_MFG_PASSED) return false;
  return true;
}

static void enter(h2_h202_mfg_t* app, h2_h202_mfg_item_t item);
static void resume(h2_h202_mfg_t* app, unsigned from);

static void fail_with(h2_h202_mfg_t* app, h2_pal_result_t error,
                      h2_h202_mfg_prompt_t prompt) {
  h2_h202_mfg_item_t item = app->item;
  app->results[item] = H2_H202_MFG_FAILED;
  app->error = error != H2_PAL_OK ? error : H2_PAL_ERR_IO;
  app->phase = H2_H202_MFG_PHASE_FAILED;
  app->deadline_set = false;
  app->pressed_since_entry = 0u;
  stop(app, H2_H202_MFG_OP_SENSOR);
  stop(app, H2_H202_MFG_OP_AUDIO);
  note(app, "H202_MFG_RESULT item=%s result=failed rc=%d", item_name(item),
       app->error);
  light(app, H2_H202_MFG_COLOR_RED);
  if (prompt != H2_H202_MFG_PROMPT_NONE) (void)play(app, prompt);
}

static void fail(h2_h202_mfg_t* app, h2_pal_result_t error) {
  fail_with(app, error, s_failures[app->item]);
}

static void pass(h2_h202_mfg_t* app) {
  h2_h202_mfg_item_t item = app->item;
  app->results[item] = H2_H202_MFG_PASSED;
  app->deadline_set = false;
  stop(app, H2_H202_MFG_OP_SENSOR);
  note(app, "H202_MFG_RESULT item=%s result=passed", item_name(item));
  if (s_successes[item] != H2_H202_MFG_PROMPT_NONE &&
      play(app, s_successes[item]) == H2_PAL_OK) {
    app->phase = H2_H202_MFG_PHASE_ANNOUNCE;
    return;
  }
  resume(app, (unsigned)item + 1u);
}

static void start_net(h2_h202_mfg_t* app) {
  h2_h202_mfg_net_t* net = &app->net;
  h2_pal_result_t rc =
      begin(app, H2_H202_MFG_OP_WIFI, H2_H202_MFG_JOB_CHECK, H2_H202_MFG_WIFI,
            H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
  net->generation = app->current[H2_H202_MFG_OP_WIFI];
  net->error = rc;
  net->state =
      rc == H2_PAL_OK ? H2_H202_MFG_NET_RUNNING : H2_H202_MFG_NET_FAILED;
  /* Each attempt or retry gets its own full budget. */
  net->deadline_ms = app->now_ms + app->config.wifi_timeout_ms;
}

/* Stage 04 reports background state; an earlier failure is announced now. */
static void net_item(h2_h202_mfg_t* app) {
  h2_h202_mfg_net_t* net = &app->net;
  switch (net->state) {
    case H2_H202_MFG_NET_IDLE:
      start_net(app);
      if (net->state == H2_H202_MFG_NET_FAILED) fail(app, net->error);
      break;
    case H2_H202_MFG_NET_RUNNING:
      /* The check can take its whole budget; tell the operator it runs. */
      if (play(app, H2_H202_MFG_PROMPT_NETWORK) != H2_PAL_OK)
        fail(app, H2_PAL_ERR_IO);
      break;
    case H2_H202_MFG_NET_PASSED:
      pass(app);
      break;
    case H2_H202_MFG_NET_FAILED:
      fail(app, net->error);
      break;
  }
}

static void accept_failed(h2_h202_mfg_t* app, h2_pal_result_t error) {
  app->error = error;
  app->phase = H2_H202_MFG_PHASE_ACCEPT_FAILED;
  app->pressed_since_entry = 0u;
  note(app, "H202_MFG_ACCEPT failed rc=%d", error);
  light(app, H2_H202_MFG_COLOR_RED);
}

static void enter_wait(h2_h202_mfg_t* app) {
  stop(app, H2_H202_MFG_OP_SENSOR);
  stop(app, H2_H202_MFG_OP_AUDIO);
  app->item = H2_H202_MFG_ITEM_COUNT;
  app->phase = H2_H202_MFG_PHASE_WAITING;
  app->deadline_set = false;
  bool passed = all_passed(app, H2_H202_MFG_ITEM_COUNT);
  note(app, "H202_MFG_STAGE stage=10 all_passed=%d", passed ? 1 : 0);
  if (!passed) {
    /* AUDIO was stopped first: an aging job checks cancellation under the
     * eye lock, so its last rainbow frame can never overwrite this color. */
    light(app, H2_H202_MFG_COLOR_YELLOW);
    (void)play(app, H2_H202_MFG_PROMPT_INCOMPLETE);
    return;
  }
  /* Boot reads and clears progress, so save a pass before announcing it:
   * installing the next firmware must not need a power cycle first. */
  app->phase = H2_H202_MFG_PHASE_ACCEPTING;
  app->pressed_since_entry = 0u;
  h2_pal_result_t rc =
      begin(app, H2_H202_MFG_OP_POWER, H2_H202_MFG_JOB_ACCEPT, app->item,
            H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
  if (rc != H2_PAL_OK) accept_failed(app, rc);
}

/* Next item not PASSED; aging is admitted only after nine PASSED results. */
static void resume(h2_h202_mfg_t* app, unsigned from) {
  for (unsigned i = from; i < H2_H202_MFG_AGING; ++i) {
    if (app->results[i] != H2_H202_MFG_PASSED) {
      enter(app, (h2_h202_mfg_item_t)i);
      return;
    }
  }
  if (from <= H2_H202_MFG_AGING &&
      app->results[H2_H202_MFG_AGING] != H2_H202_MFG_PASSED &&
      all_passed(app, H2_H202_MFG_AGING)) {
    enter(app, H2_H202_MFG_AGING);
    return;
  }
  enter_wait(app);
}

static void enter(h2_h202_mfg_t* app, h2_h202_mfg_item_t item) {
  stop(app, H2_H202_MFG_OP_SENSOR);
  stop(app, H2_H202_MFG_OP_AUDIO);
  app->item = item;
  app->phase = H2_H202_MFG_PHASE_ACTIVE;
  app->deadline_set = false;
  app->pressed_since_entry = 0u;
  app->error = H2_PAL_OK;
  note(app, "H202_MFG_STAGE stage=%u item=%s", h2_h202_mfg_stage_of(item),
       item_name(item));
  if (item != H2_H202_MFG_AGING) light(app, s_colors[item]);
  h2_pal_result_t rc = H2_PAL_OK;
  switch (item) {
    case H2_H202_MFG_CHARGER:
      app->charger_initial = -1;
      app->charger_edges = 0u;
      app->battery_present = false;
      rc = begin(app, H2_H202_MFG_OP_SENSOR, H2_H202_MFG_JOB_CHARGER, item,
                 H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
      app->deadline_ms = app->now_ms + app->config.charger_timeout_ms;
      app->deadline_set = true;
      break;
    case H2_H202_MFG_NFC:
      rc = play(app, s_instructions[item]);
      if (rc == H2_PAL_OK)
        rc = begin(app, H2_H202_MFG_OP_SENSOR, H2_H202_MFG_JOB_NFC, item,
                   H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
      app->deadline_ms = app->now_ms + app->config.nfc_timeout_ms;
      app->deadline_set = true;
      break;
    case H2_H202_MFG_WIFI:
      net_item(app);
      return;
    case H2_H202_MFG_LEFT_MIC:
    case H2_H202_MFG_RIGHT_MIC:
      /* The job plays its instruction, then registers the observer with
       * microphone and speaker stopped and measures. */
      rc = begin(app, H2_H202_MFG_OP_AUDIO, H2_H202_MFG_JOB_MIC, item,
                 s_instructions[item], H2_H202_MFG_PROMPT_NONE);
      break;
    case H2_H202_MFG_AGING:
      rc = begin(app, H2_H202_MFG_OP_AUDIO, H2_H202_MFG_JOB_AGING, item,
                 s_instructions[item], H2_H202_MFG_PROMPT_NONE);
      /* The rainbow runs on its own worker: LED writes block, and the audio
       * loop must keep up with capture or the board latches a fault. */
      if (rc == H2_PAL_OK)
        rc = begin(app, H2_H202_MFG_OP_SENSOR, H2_H202_MFG_JOB_RAINBOW, item,
                   H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
      /* Wi-Fi restarts and keeps reconnecting; its losses and results are
       * diagnostics only and never revoke an earlier PASS. */
      app->maintaining = true;
      (void)begin(app, H2_H202_MFG_OP_WIFI, H2_H202_MFG_JOB_MAINTAIN,
                  H2_H202_MFG_WIFI, H2_H202_MFG_PROMPT_NONE,
                  H2_H202_MFG_PROMPT_NONE);
      break;
    default:
      rc = play(app, s_instructions[item]);
      break;
  }
  if (rc != H2_PAL_OK) fail(app, rc);
}

static void retry(h2_h202_mfg_t* app) {
  note(app, "H202_MFG_RETRY item=%s", item_name(app->item));
  if (app->item == H2_H202_MFG_WIFI) start_net(app);
  enter(app, app->item);
}

static void skip(h2_h202_mfg_t* app) {
  if (app->phase != H2_H202_MFG_PHASE_ACTIVE &&
      app->phase != H2_H202_MFG_PHASE_FAILED &&
      app->phase != H2_H202_MFG_PHASE_STOPPING)
    return;
  h2_h202_mfg_item_t item = app->item;
  app->results[item] = H2_H202_MFG_SKIPPED;
  stop(app, H2_H202_MFG_OP_SENSOR);
  stop(app, H2_H202_MFG_OP_AUDIO);
  if (item == H2_H202_MFG_WIFI) {
    stop(app, H2_H202_MFG_OP_WIFI);
    app->net.state = H2_H202_MFG_NET_IDLE;
  }
  note(app, "H202_MFG_RESULT item=%s result=skipped", item_name(item));
  resume(app, (unsigned)item + 1u);
}

static void shutdown(h2_h202_mfg_t* app) {
  if (app->phase == H2_H202_MFG_PHASE_SAVING) return;
  for (unsigned op = 0; op < H2_H202_MFG_OP_POWER; ++op)
    stop(app, (h2_h202_mfg_operation_t)op);
  app->phase = H2_H202_MFG_PHASE_SAVING;
  app->deadline_set = false;
  app->pressed_since_entry = 0u;
  app->committed = false;
  note(app, "H202_MFG_SHUTDOWN begin");
  h2_pal_result_t rc =
      begin(app, H2_H202_MFG_OP_POWER, H2_H202_MFG_JOB_SHUTDOWN, app->item,
            H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
  if (rc != H2_PAL_OK) {
    app->error = rc;
    app->phase = H2_H202_MFG_PHASE_UNSAVED;
    light(app, H2_H202_MFG_COLOR_RED);
    (void)play(app, H2_H202_MFG_PROMPT_UNSAVED);
  }
}

static void reset_failed(h2_h202_mfg_t* app, h2_pal_result_t error) {
  app->error = error;
  app->phase = H2_H202_MFG_PHASE_RESET_FAILED;
  app->pressed_since_entry = 0u;
  note(app, "H202_MFG_RESET failed rc=%d", error);
  light(app, H2_H202_MFG_COLOR_RED);
}

/* Record + short power: drop the saved record and reboot into stage 02. */
static void reset_progress(h2_h202_mfg_t* app) {
  if (app->phase == H2_H202_MFG_PHASE_SAVING ||
      app->phase == H2_H202_MFG_PHASE_RESETTING)
    return;
  for (unsigned op = 0; op < H2_H202_MFG_OP_POWER; ++op)
    stop(app, (h2_h202_mfg_operation_t)op);
  app->phase = H2_H202_MFG_PHASE_RESETTING;
  app->deadline_set = false;
  app->pressed_since_entry = 0u;
  note(app, "H202_MFG_RESET begin");
  h2_pal_result_t rc =
      begin(app, H2_H202_MFG_OP_POWER, H2_H202_MFG_JOB_RESET, app->item,
            H2_H202_MFG_PROMPT_NONE, H2_H202_MFG_PROMPT_NONE);
  if (rc != H2_PAL_OK) reset_failed(app, rc);
}

/* Stage 10 Volume -: page back and run aging again. Aging admission still
 * needs the nine earlier results PASSED; a saved pass stays saved until the
 * next stage 10 or shutdown save. */
static void back_to_aging(h2_h202_mfg_t* app) {
  if (!all_passed(app, H2_H202_MFG_AGING)) {
    note(app, "H202_MFG_BACK refused");
    return;
  }
  note(app, "H202_MFG_BACK item=aging");
  app->results[H2_H202_MFG_AGING] = H2_H202_MFG_UNTESTED;
  enter(app, H2_H202_MFG_AGING);
}

static void click(h2_h202_mfg_t* app, unsigned key) {
  if ((app->pressed_since_entry & key) == 0u) return;
  app->pressed_since_entry &= ~key;
  switch (app->phase) {
    case H2_H202_MFG_PHASE_FAILED:
      if (key == H2_H202_MFG_KEY_RECORD) retry(app);
      return;
    case H2_H202_MFG_PHASE_UNSAVED:
    case H2_H202_MFG_PHASE_POWEROFF_FAILED:
      if (key == H2_H202_MFG_KEY_RECORD) shutdown(app);
      return;
    case H2_H202_MFG_PHASE_RESET_FAILED:
      if (key == H2_H202_MFG_KEY_RECORD) reset_progress(app);
      return;
    case H2_H202_MFG_PHASE_ACCEPT_FAILED:
      if (key == H2_H202_MFG_KEY_RECORD) enter_wait(app);
      if (key == H2_H202_MFG_KEY_MINUS) back_to_aging(app);
      return;
    case H2_H202_MFG_PHASE_WAITING:
      if (key == H2_H202_MFG_KEY_MINUS) back_to_aging(app);
      return;
    case H2_H202_MFG_PHASE_ACTIVE:
      break;
    default:
      return;
  }
  switch (app->item) {
    case H2_H202_MFG_LISTEN:
      if (key == H2_H202_MFG_KEY_RECORD) pass(app);
      break;
    case H2_H202_MFG_POWER:
      if (key == H2_H202_MFG_KEY_POWER) pass(app);
      break;
    case H2_H202_MFG_PLUS:
      if (key == H2_H202_MFG_KEY_PLUS) pass(app);
      break;
    case H2_H202_MFG_MINUS:
      if (key == H2_H202_MFG_KEY_MINUS) pass(app);
      break;
    case H2_H202_MFG_AGING:
      if ((key == H2_H202_MFG_KEY_PLUS || key == H2_H202_MFG_KEY_MINUS) &&
          app->config.callbacks.aging_volume != NULL) {
        app->config.callbacks.aging_volume(
            app->config.callbacks.user, key == H2_H202_MFG_KEY_PLUS ? 20 : -20);
      } else if (key == H2_H202_MFG_KEY_RECORD &&
                 app->current[H2_H202_MFG_OP_AUDIO] != 0u) {
        /* Keep the generation: the job's DONE is the clean-stop evidence. */
        app->phase = H2_H202_MFG_PHASE_STOPPING;
        note(app, "H202_MFG_AGING stop");
        app->config.callbacks.cancel(app->config.callbacks.user,
                                     H2_H202_MFG_OP_AUDIO,
                                     app->current[H2_H202_MFG_OP_AUDIO]);
      }
      break;
    default:
      break;
  }
}

static void keys_changed(h2_h202_mfg_t* app, unsigned keys) {
  unsigned pressed = keys & ~app->keys;
  unsigned released = app->keys & ~keys;
  app->keys = keys;
  app->pressed_since_entry |= pressed;
  if (pressed & H2_H202_MFG_KEY_RECORD) app->record_chord = false;
  if (pressed & H2_H202_MFG_KEY_POWER) {
    app->power_held = true;
    app->power_down_ms = app->now_ms;
    if (keys & H2_H202_MFG_KEY_RECORD) app->record_chord = true;
  }
  if (pressed & H2_H202_MFG_KEY_PLUS) {
    app->plus_chord = (keys & H2_H202_MFG_KEY_RECORD) != 0u;
    if (app->plus_chord) app->record_chord = true;
  }
  if (released & H2_H202_MFG_KEY_POWER) {
    bool held = app->power_held;
    bool hold = app->now_ms - app->power_down_ms >= app->config.power_hold_ms;
    app->power_held = false;
    if (keys & H2_H202_MFG_KEY_RECORD) {
      /* Power with Record held is never a power press and consumes Record;
       * only a short press resets, a long one does nothing. */
      app->record_chord = true;
      app->pressed_since_entry &= ~H2_H202_MFG_KEY_POWER;
      if (held && !hold) reset_progress(app);
    } else if (held && hold) {
      shutdown(app);
    } else if (held) {
      click(app, H2_H202_MFG_KEY_POWER);
    }
  }
  if (released & H2_H202_MFG_KEY_PLUS) {
    /* Plus with Record held is never a Volume + press and consumes Record. */
    if (app->plus_chord || (keys & H2_H202_MFG_KEY_RECORD)) {
      bool gesture = app->plus_chord && (keys & H2_H202_MFG_KEY_RECORD) &&
                     (app->pressed_since_entry & H2_H202_MFG_KEY_PLUS);
      app->record_chord = true;
      app->pressed_since_entry &= ~H2_H202_MFG_KEY_PLUS;
      if (gesture) skip(app);
    } else {
      click(app, H2_H202_MFG_KEY_PLUS);
    }
    app->plus_chord = false;
  }
  if (released & H2_H202_MFG_KEY_RECORD) {
    if (app->record_chord)
      app->pressed_since_entry &= ~H2_H202_MFG_KEY_RECORD;
    else
      click(app, H2_H202_MFG_KEY_RECORD);
    app->record_chord = false;
  }
  if (released & H2_H202_MFG_KEY_MINUS) click(app, H2_H202_MFG_KEY_MINUS);
}

static void audio_done(h2_h202_mfg_t* app, const h2_h202_mfg_completion_t* c) {
  switch (app->audio_job) {
    case H2_H202_MFG_JOB_PROMPT:
      if (c->result != H2_PAL_OK) {
        note(app, "H202_MFG_PROMPT prompt=%d rc=%d", app->prompt, c->result);
        if (app->phase == H2_H202_MFG_PHASE_ACTIVE) {
          fail(app, c->result);
          return;
        }
      }
      if (app->phase == H2_H202_MFG_PHASE_ANNOUNCE)
        resume(app, (unsigned)app->item + 1u);
      return;
    case H2_H202_MFG_JOB_MIC:
      if (app->phase != H2_H202_MFG_PHASE_ACTIVE) return;
      note(app, "H202_MFG_MIC item=%s rc=%d verdict=%d", item_name(app->item),
           c->result, (int)c->value);
      /* Levels are diagnostics; the operator confirms with Record. */
      if (c->result == H2_PAL_OK)
        pass(app);
      else
        fail(app, c->result);
      return;
    case H2_H202_MFG_JOB_AGING:
      /* Passes only after a requested stop finished cleanly. */
      if (app->phase == H2_H202_MFG_PHASE_STOPPING && c->result == H2_PAL_OK)
        pass(app);
      else if (app->phase == H2_H202_MFG_PHASE_STOPPING ||
               app->phase == H2_H202_MFG_PHASE_ACTIVE)
        fail(app, c->result != H2_PAL_OK ? c->result : H2_PAL_ERR_IO);
      return;
    default:
      return;
  }
}

static void sensor_event(h2_h202_mfg_t* app,
                         const h2_h202_mfg_completion_t* c) {
  if (app->phase != H2_H202_MFG_PHASE_ACTIVE) return;
  /* Battery and charger are checked together in stage 02; a missing
   * battery is announced at once with its own prompt. */
  if (c->event == H2_H202_MFG_EVENT_BATTERY &&
      app->item == H2_H202_MFG_CHARGER) {
    app->battery_present = c->result == H2_PAL_OK && c->value != 0;
    if (!app->battery_present)
      fail_with(app, c->result != H2_PAL_OK ? c->result : H2_PAL_ERR_NOT_FOUND,
                H2_H202_MFG_PROMPT_BATTERY_FAILED);
    return;
  }
  if (c->event == H2_H202_MFG_EVENT_CHARGER &&
      app->item == H2_H202_MFG_CHARGER) {
    bool inserted = c->value != 0;
    if (app->charger_initial < 0) {
      app->charger_initial = inserted ? 1 : 0;
    } else if (++app->charger_edges >= 2u) {
      if (app->battery_present)
        pass(app);
      else
        fail_with(app, H2_PAL_ERR_NOT_FOUND, H2_H202_MFG_PROMPT_BATTERY_FAILED);
      return;
    }
    if (play(app, inserted ? H2_H202_MFG_PROMPT_CHARGE_REMOVE
                           : H2_H202_MFG_PROMPT_CHARGE_INSERT) != H2_PAL_OK)
      fail(app, H2_PAL_ERR_IO);
    return;
  }
  if (c->event != H2_H202_MFG_EVENT_DONE) return;
  app->current[H2_H202_MFG_OP_SENSOR] = 0u;
  if (app->item == H2_H202_MFG_NFC && c->result == H2_PAL_OK)
    pass(app);
  else if (app->item == H2_H202_MFG_NFC || app->item == H2_H202_MFG_CHARGER)
    fail(app, c->result);
  else if (app->item == H2_H202_MFG_AGING && c->result != H2_PAL_OK)
    fail(app, c->result); /* RGB error. */
}

static void net_done(h2_h202_mfg_t* app, const h2_h202_mfg_completion_t* c) {
  app->current[H2_H202_MFG_OP_WIFI] = 0u;
  if (app->maintaining) {
    note(app, "H202_MFG_NETWORK op=wifi maintain rc=%d", c->result);
    return;
  }
  h2_h202_mfg_net_t* net = &app->net;
  if (net->state != H2_H202_MFG_NET_RUNNING) return;
  net->state =
      c->result == H2_PAL_OK ? H2_H202_MFG_NET_PASSED : H2_H202_MFG_NET_FAILED;
  net->error = c->result;
  note(app, "H202_MFG_NETWORK op=wifi rc=%d", c->result);
  if (app->phase == H2_H202_MFG_PHASE_ACTIVE && app->item == H2_H202_MFG_WIFI)
    net_item(app);
}

static void power_event(h2_h202_mfg_t* app, const h2_h202_mfg_completion_t* c) {
  if (c->event == H2_H202_MFG_EVENT_DONE) { /* Only ACCEPT ends with DONE. */
    app->current[H2_H202_MFG_OP_POWER] = 0u;
    if (app->phase != H2_H202_MFG_PHASE_ACCEPTING) return;
    if (c->result != H2_PAL_OK) {
      accept_failed(app, c->result);
      return;
    }
    note(app, "H202_MFG_ACCEPTED");
    app->phase = H2_H202_MFG_PHASE_WAITING;
    light(app, H2_H202_MFG_COLOR_GREEN);
    (void)play(app, H2_H202_MFG_PROMPT_WAITING);
    return;
  }
  if (app->phase == H2_H202_MFG_PHASE_RESETTING) {
    /* A successful reboot never reports back. */
    app->current[H2_H202_MFG_OP_POWER] = 0u;
    reset_failed(app,
                 c->result != H2_PAL_OK ? c->result : H2_PAL_ERR_INVALID_STATE);
    return;
  }
  if (app->phase != H2_H202_MFG_PHASE_SAVING) return;
  if (c->event == H2_H202_MFG_EVENT_COMMITTED && c->result == H2_PAL_OK) {
    app->committed = true;
    note(app, "H202_MFG_SHUTDOWN committed");
    return;
  }
  app->current[H2_H202_MFG_OP_POWER] = 0u;
  app->error = c->result;
  app->pressed_since_entry = 0u;
  bool unsaved = c->event == H2_H202_MFG_EVENT_COMMITTED || !app->committed;
  app->phase =
      unsaved ? H2_H202_MFG_PHASE_UNSAVED : H2_H202_MFG_PHASE_POWEROFF_FAILED;
  note(app, "H202_MFG_SHUTDOWN failed unsaved=%d rc=%d", unsaved ? 1 : 0,
       c->result);
  light(app, H2_H202_MFG_COLOR_RED);
  (void)play(app, unsaved ? H2_H202_MFG_PROMPT_UNSAVED
                          : H2_H202_MFG_PROMPT_POWEROFF_FAILED);
}

static void completion(h2_h202_mfg_t* app, const h2_h202_mfg_completion_t* c) {
  if ((unsigned)c->operation >= H2_H202_MFG_OP_COUNT) return;
  if (c->generation == 0u || c->generation != app->current[c->operation]) {
    note(app, "H202_MFG_STALE op=%d generation=%u", c->operation,
         (unsigned)c->generation);
    return;
  }
  switch (c->operation) {
    case H2_H202_MFG_OP_AUDIO:
      if (c->event != H2_H202_MFG_EVENT_DONE) return;
      app->current[H2_H202_MFG_OP_AUDIO] = 0u;
      audio_done(app, c);
      return;
    case H2_H202_MFG_OP_SENSOR:
      sensor_event(app, c);
      return;
    case H2_H202_MFG_OP_WIFI:
      if (c->event == H2_H202_MFG_EVENT_DONE) net_done(app, c);
      return;
    case H2_H202_MFG_OP_POWER:
      power_event(app, c);
      return;
    default:
      return;
  }
}

static void deadlines(h2_h202_mfg_t* app) {
  h2_h202_mfg_net_t* net = &app->net;
  if (!app->maintaining && net->state == H2_H202_MFG_NET_RUNNING &&
      app->now_ms >= net->deadline_ms) {
    stop(app, H2_H202_MFG_OP_WIFI);
    net->state = H2_H202_MFG_NET_FAILED;
    net->error = H2_PAL_ERR_TIMEOUT;
    note(app, "H202_MFG_NETWORK op=wifi timeout");
    if (app->phase == H2_H202_MFG_PHASE_ACTIVE && app->item == H2_H202_MFG_WIFI)
      net_item(app);
  }
  if (app->phase == H2_H202_MFG_PHASE_ACTIVE && app->deadline_set &&
      app->now_ms >= app->deadline_ms)
    fail(app, H2_PAL_ERR_TIMEOUT);
  /* With Record held the power key is the reset chord, never shutdown. */
  if (app->power_held && (app->keys & H2_H202_MFG_KEY_POWER) &&
      !(app->keys & H2_H202_MFG_KEY_RECORD) &&
      app->now_ms - app->power_down_ms >= app->config.power_hold_ms) {
    app->power_held = false;
    shutdown(app);
  }
}

h2_pal_result_t h2_h202_mfg_init(h2_h202_mfg_t* app,
                                 const h2_h202_mfg_config_t* config) {
  if (app == NULL || config == NULL || config->callbacks.begin == NULL ||
      config->callbacks.cancel == NULL || config->callbacks.light == NULL ||
      config->power_hold_ms == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  memset(app, 0, sizeof(*app));
  app->config = *config;
  app->phase = H2_H202_MFG_PHASE_IDLE;
  app->prompt = H2_H202_MFG_PROMPT_NONE;
  app->charger_initial = -1;
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_mfg_start(h2_h202_mfg_t* app,
                                  const h2_h202_mfg_restore_t* restore,
                                  uint64_t now_ms, unsigned keys) {
  if (app == NULL || (keys & ~(unsigned)H2_H202_MFG_KEY_MASK) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (app->phase != H2_H202_MFG_PHASE_IDLE) return H2_PAL_ERR_INVALID_STATE;
  app->now_ms = now_ms;
  app->keys = keys; /* Keys already held need a new press. */
  if (restore != NULL) {
    app->restore_error = restore->error;
    if (restore->error == H2_PAL_OK && restore->present)
      for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
        if (restore->results[i] == H2_H202_MFG_PASSED)
          app->results[i] = H2_H202_MFG_PASSED;
  }
  note(app, "H202_MFG_STAGE stage=1 restore=%d", app->restore_error);
  if (app->results[H2_H202_MFG_WIFI] != H2_H202_MFG_PASSED) start_net(app);
  resume(app, H2_H202_MFG_CHARGER);
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_mfg_update(h2_h202_mfg_t* app, uint64_t now_ms,
                                   unsigned keys,
                                   const h2_h202_mfg_completion_t* completions,
                                   size_t count) {
  if (app == NULL || (completions == NULL && count != 0u) ||
      (keys & ~(unsigned)H2_H202_MFG_KEY_MASK) != 0u)
    return H2_PAL_ERR_INVALID_ARG;
  if (app->phase == H2_H202_MFG_PHASE_IDLE) return H2_PAL_ERR_INVALID_STATE;
  if (now_ms < app->now_ms) return H2_PAL_ERR_INVALID_ARG;
  app->now_ms = now_ms;
  if (keys != app->keys) keys_changed(app, keys);
  for (size_t i = 0; i < count; ++i) completion(app, &completions[i]);
  deadlines(app);
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_mfg_status(const h2_h202_mfg_t* app,
                                   h2_h202_mfg_status_t* out) {
  if (app == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = (h2_h202_mfg_status_t){.phase = app->phase,
                                .stage = app->phase == H2_H202_MFG_PHASE_IDLE
                                             ? 1u
                                             : h2_h202_mfg_stage_of(app->item),
                                .item = app->item,
                                .wifi = app->net.state,
                                .error = app->error,
                                .restore_error = app->restore_error,
                                .color = app->color,
                                .prompt = app->prompt,
                                .committed = app->committed};
  memcpy(out->results, app->results, sizeof(out->results));
  return H2_PAL_OK;
}

/* ---- Progress --------------------------------------------------------- */

void h2_h202_mfg_progress_load(const h2_pal_pref_api_t* pref,
                               const h2_pal_mem_api_t* mem,
                               h2_h202_mfg_restore_t* out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  /* An incompatible revision or total resets the record here. */
  int rc = h2_loader_mfg_ensure_acceptance_revision(
      pref, H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_REVISION);
  h2_loader_mfg_summary_t summary;
  memset(&summary, 0, sizeof(summary));
  int present = 0;
  if (rc == H2_PAL_OK) rc = h2_loader_mfg_read(pref, mem, &summary, &present);
  if (rc == H2_PAL_OK && present && summary.total == H2_H202_MFG_ITEM_COUNT) {
    out->present = true;
    for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
      out->results[i] = summary.step_status[i] <= H2_H202_MFG_FAILED
                            ? summary.step_status[i]
                            : H2_H202_MFG_UNTESTED;
  }
  /* One-shot: clear before any stage runs, so reset or power loss restarts
   * at stage 02. A failed clear discards what was read. */
  int cleared = h2_loader_mfg_reset(pref, H2_H202_MFG_ITEM_COUNT);
  if (rc == H2_PAL_OK) rc = cleared;
  if (rc != H2_PAL_OK) {
    memset(out, 0, sizeof(*out));
    out->error = (h2_pal_result_t)rc;
  }
}

h2_pal_result_t h2_h202_mfg_acceptance_gate(h2_loader_t* loader,
                                            const h2_pal_pref_api_t* pref) {
  if (loader == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_loader_mfg_ensure_acceptance_revision(
      pref, H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_REVISION);
  /* force_command_mode only stops automatic startup; reboot app follows
   * command availability and the stored results. */
  loader->force_command_mode = rc != H2_PAL_OK;
  int closed = h2_loader_set_command_availability(
      loader, H2_LOADER_COMMAND_AVAILABLE_REBOOT_APP, rc == H2_PAL_OK);
  return (h2_pal_result_t)(rc != H2_PAL_OK ? rc : closed);
}

h2_pal_result_t h2_h202_mfg_progress_clear(const h2_pal_pref_api_t* pref) {
  return (h2_pal_result_t)h2_loader_mfg_reset(pref, H2_H202_MFG_ITEM_COUNT);
}

h2_pal_result_t h2_h202_mfg_progress_commit(
    const h2_pal_pref_api_t* pref,
    const uint8_t results[H2_H202_MFG_ITEM_COUNT]) {
  if (results == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_loader_mfg_summary_t summary;
  memset(&summary, 0, sizeof(summary));
  summary.total = H2_H202_MFG_ITEM_COUNT;
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i) {
    if (results[i] > H2_H202_MFG_FAILED) return H2_PAL_ERR_INVALID_ARG;
    summary.step_status[i] = results[i];
  }
  return (h2_pal_result_t)h2_loader_mfg_write(pref, &summary);
}

/* ---- Charge indication ------------------------------------------------ */

void h2_h202_mfg_charge_init(h2_h202_mfg_charge_t* charge, uint32_t window_ms,
                             unsigned toggles, uint32_t blink_period_ms,
                             uint32_t breathe_period_ms) {
  if (charge == NULL) return;
  memset(charge, 0, sizeof(*charge));
  charge->window_ms = window_ms;
  charge->toggles = toggles;
  charge->blink_period_ms = blink_period_ms != 0u ? blink_period_ms : 1u;
  charge->breathe_period_ms =
      breathe_period_ms >= 2u ? breathe_period_ms : 2000u;
}

h2_h202_mfg_charge_mode_t h2_h202_mfg_charge_update(
    h2_h202_mfg_charge_t* charge, uint64_t now_ms, bool usb_insert,
    bool charging, bool done, uint16_t* duty_x100) {
  enum { HISTORY = sizeof(charge->changes) / sizeof(charge->changes[0]) };
  if (!charge->have_chrg) {
    charge->have_chrg = true;
    charge->chrg = charging;
  } else if (charging != charge->chrg) {
    charge->chrg = charging;
    charge->changes[charge->change_count % HISTORY] = now_ms;
    ++charge->change_count;
  }
  unsigned recent = 0u;
  unsigned stored =
      charge->change_count < HISTORY ? charge->change_count : (unsigned)HISTORY;
  for (unsigned i = 0; i < stored; ++i)
    if (now_ms - charge->changes[i] < charge->window_ms) ++recent;
  bool toggling = charge->toggles != 0u && recent >= charge->toggles;
  h2_h202_mfg_charge_mode_t mode;
  if (!usb_insert)
    mode = H2_H202_MFG_CHARGE_OFF;
  else if (toggling && done) /* No battery: LGS4056 CHRG blinks. */
    mode = H2_H202_MFG_CHARGE_BLINK;
  else if (charging && !done)
    mode = H2_H202_MFG_CHARGE_BREATHE;
  else if (!charging && done) /* Full. */
    mode = H2_H202_MFG_CHARGE_ON;
  else /* Both off: undervoltage or temperature fault. Both on: abnormal. */
    mode = H2_H202_MFG_CHARGE_BLINK;
  if (duty_x100 != NULL) {
    uint16_t duty = 0u;
    if (mode == H2_H202_MFG_CHARGE_BREATHE) {
      /* A full rise and fall per period, calculated from monotonic time so
       * delayed controller ticks cannot stretch the cycle. */
      uint32_t period = charge->breathe_period_ms;
      uint32_t phase = (uint32_t)(now_ms % period);
      uint32_t rise = period / 2u;
      duty =
          (uint16_t)(phase <= rise ? (uint64_t)phase *
                                         H2_PAL_PWM_SWITCH_DUTY_MAX_X100 / rise
                                   : (uint64_t)(period - phase) *
                                         H2_PAL_PWM_SWITCH_DUTY_MAX_X100 /
                                         (period - rise));
    } else if (mode == H2_H202_MFG_CHARGE_ON ||
               (mode == H2_H202_MFG_CHARGE_BLINK &&
                now_ms % charge->blink_period_ms <
                    charge->blink_period_ms / 2u)) {
      duty = H2_PAL_PWM_SWITCH_DUTY_MAX_X100;
    }
    *duty_x100 = duty;
  }
  return mode;
}

/* ---- Stage 05 capture check ------------------------------------------ */

/* Broadband level in dBFS*10, where 0 is full-scale DC. */
static int16_t energy_db(float sum_squares, uint32_t samples) {
  float power = sum_squares / (float)samples;
  if (power < 1e-12f) return -1200;
  float db = 100.0f * log10f(power);
  return (int16_t)(db < -1200.0f ? -1200 : db > 0.0f ? 0 : db);
}

h2_pal_result_t h2_h202_mfg_aec_meter_init(
    h2_h202_mfg_aec_meter_t* meter, const h2_h202_mfg_aec_config_t* config,
    uint8_t lane) {
  if (meter == NULL || config == NULL || lane > 1u ||
      config->sample_rate_hz == 0u || config->block_ms < 100u ||
      config->measure_ms < config->block_ms ||
      config->measure_ms / config->block_ms > H2_H202_MFG_AEC_MAX_BLOCKS)
    return H2_PAL_ERR_INVALID_ARG;
  memset(meter, 0, sizeof(*meter));
  meter->config = *config;
  meter->lane = lane;
  meter->block_samples =
      (uint32_t)((uint64_t)config->sample_rate_hz * config->block_ms / 1000u);
  return H2_PAL_OK;
}

static uint64_t meter_samples(const h2_h202_mfg_aec_meter_t* meter,
                              uint32_t ms) {
  return (uint64_t)meter->config.sample_rate_hz * ms / 1000u;
}

static bool pcm_frame(const h2_audio_frame_t* frame, unsigned channels,
                      uint16_t samples) {
  return frame != NULL && frame->data != NULL &&
         frame->sample_format == H2_AUDIO_SAMPLE_S16LE &&
         frame->channels == channels && frame->samples_per_channel == samples &&
         frame->bytes >= (size_t)samples * channels * sizeof(int16_t);
}

void h2_h202_mfg_aec_meter_capture(h2_h202_mfg_aec_meter_t* meter,
                                   const h2_audio_aec_frame_t* frame) {
  if (meter == NULL || frame == NULL || frame->microphone_lane != meter->lane ||
      frame->raw == NULL)
    return;
  uint16_t samples = frame->raw->samples_per_channel;
  unsigned channels = frame->raw->channels;
  if (samples == 0u || meter->lane >= channels ||
      !pcm_frame(frame->raw, channels, samples) ||
      !pcm_frame(frame->reference_input, 1u, samples) ||
      !pcm_frame(frame->processed, 1u, samples))
    return;
  const int16_t* raw = frame->raw->data;
  const int16_t* reference = frame->reference_input->data;
  const int16_t* processed = frame->processed->data;
  uint64_t start = meter_samples(meter, meter->config.convergence_ms);
  uint64_t end = start + meter_samples(meter, meter->config.measure_ms);
  ++meter->lane_frames;
  for (unsigned i = 0; i < samples; ++i) {
    int16_t x = raw[i * channels + meter->lane];
    uint64_t position = meter->lane_samples++;
    if (position < start || position >= end ||
        meter->blocks >= H2_H202_MFG_AEC_MAX_BLOCKS)
      continue;
    if (x != 0) meter->nonzero = true;
    if (processed[i] != 0) meter->processed_nonzero = true;
    /* No known excitation: broadband levels only, never a claimed echo
     * reduction. Audible quality is confirmed by the operator. */
    float input = (float)x / 32768.0f;
    float ref = (float)reference[i] / 32768.0f;
    float output = (float)processed[i] / 32768.0f;
    meter->raw += input * input;
    meter->reference += ref * ref;
    meter->processed += output * output;
    if (++meter->in_block < meter->block_samples) continue;
    meter->raw_db_x10[meter->blocks] =
        energy_db(meter->raw, meter->block_samples);
    meter->reference_db_x10[meter->blocks] =
        energy_db(meter->reference, meter->block_samples);
    meter->processed_db_x10[meter->blocks] =
        energy_db(meter->processed, meter->block_samples);
    ++meter->blocks;
    meter->in_block = 0u;
    meter->raw = meter->reference = meter->processed = 0.0f;
  }
}

bool h2_h202_mfg_aec_meter_done(const h2_h202_mfg_aec_meter_t* meter) {
  return meter != NULL &&
         meter->lane_samples >=
             meter_samples(meter, meter->config.convergence_ms) +
                 meter_samples(meter, meter->config.measure_ms);
}

static int32_t median(const int16_t* values, unsigned count) {
  int16_t sorted[H2_H202_MFG_AEC_MAX_BLOCKS];
  memcpy(sorted, values, count * sizeof(values[0]));
  for (unsigned i = 1; i < count; ++i) {
    int16_t value = sorted[i];
    unsigned j = i;
    for (; j > 0 && sorted[j - 1] > value; --j) sorted[j] = sorted[j - 1];
    sorted[j] = value;
  }
  return count % 2u ? sorted[count / 2u]
                    : (sorted[count / 2u - 1u] + sorted[count / 2u]) / 2;
}

h2_h202_mfg_aec_verdict_t h2_h202_mfg_aec_meter_result(
    const h2_h202_mfg_aec_meter_t* meter, h2_h202_mfg_aec_result_t* out) {
  h2_h202_mfg_aec_result_t result = {.verdict = H2_H202_MFG_AEC_NO_SIGNAL};
  if (meter != NULL && meter->lane_frames != 0u && meter->nonzero) {
    result.blocks = meter->blocks;
    if (meter->blocks == 0u || !h2_h202_mfg_aec_meter_done(meter)) {
      result.verdict = meter->blocks == 0u ? H2_H202_MFG_AEC_NO_SIGNAL
                                           : H2_H202_MFG_AEC_INCOMPLETE;
    } else {
      result.raw_dbfs_x10 = median(meter->raw_db_x10, meter->blocks);
      result.reference_dbfs_x10 =
          median(meter->reference_db_x10, meter->blocks);
      result.processed_dbfs_x10 =
          median(meter->processed_db_x10, meter->blocks);
      result.verdict = meter->processed_nonzero ? H2_H202_MFG_AEC_PASS
                                                : H2_H202_MFG_AEC_NO_SIGNAL;
    }
  }
  if (out != NULL) *out = result;
  return result.verdict;
}

/* ---- 8 kHz prompt playback -------------------------------------------- */

enum { PROMPT_INPUT = 160, PROMPT_OUTPUT = PROMPT_INPUT * 2 };

void h2_h202_mfg_upsample(const int16_t* in, size_t count, int16_t* out,
                          int16_t* state) {
  int32_t previous = *state;
  for (size_t i = 0; i < count; ++i) {
    out[i * 2u] = (int16_t)((previous + in[i]) / 2);
    out[i * 2u + 1u] = in[i];
    previous = in[i];
  }
  *state = (int16_t)previous;
}

/* Standard IMA ADPCM tables; prompts/ima_adpcm.py encodes with the same. */
static const int16_t s_adpcm_steps[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t s_adpcm_index[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

int16_t h2_h202_mfg_adpcm_next(h2_h202_mfg_adpcm_state_t* state,
                               unsigned nibble) {
  const int32_t step = s_adpcm_steps[state->index];
  int32_t diff = step >> 3;
  if (nibble & 4u) diff += step;
  if (nibble & 2u) diff += step >> 1;
  if (nibble & 1u) diff += step >> 2;
  int32_t predictor =
      (nibble & 8u) ? state->predictor - diff : state->predictor + diff;
  if (predictor > INT16_MAX) predictor = INT16_MAX;
  if (predictor < INT16_MIN) predictor = INT16_MIN;
  int32_t index = state->index + s_adpcm_index[nibble & 7u];
  state->index = index < 0 ? 0 : index > 88 ? 88 : index;
  state->predictor = predictor;
  return (int16_t)predictor;
}

h2_pal_result_t h2_h202_mfg_play_adpcm8k(h2_pal_audio_track_t* track,
                                         const unsigned char* adpcm,
                                         size_t bytes,
                                         bool (*canceled)(void* user),
                                         void* user) {
  if (track == NULL || (adpcm == NULL && bytes != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  int16_t input[PROMPT_INPUT];
  int16_t output[PROMPT_OUTPUT];
  int16_t state = 0;
  h2_h202_mfg_adpcm_state_t decoder = {0};
  const h2_audio_pcm_format_t format = {
      .sample_rate_hz = 16000u,
      .frame_samples_per_channel = PROMPT_OUTPUT,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE};
  h2_audio_frame_t frame =
      h2_audio_frame_for_buffer(output, sizeof(output), format);
  frame.bytes = sizeof(output);
  for (size_t offset = 0; offset < bytes; offset += PROMPT_INPUT / 2u) {
    if (canceled != NULL && canceled(user)) return H2_PAL_ERR_CLOSED;
    size_t count = bytes - offset;
    if (count > PROMPT_INPUT / 2u) count = PROMPT_INPUT / 2u;
    for (size_t i = 0; i < count; ++i) {
      input[i * 2u] = h2_h202_mfg_adpcm_next(&decoder, adpcm[offset + i] & 15u);
      input[i * 2u + 1u] =
          h2_h202_mfg_adpcm_next(&decoder, adpcm[offset + i] >> 4);
    }
    memset(output, 0, sizeof(output)); /* Final partial frame pads silence. */
    h2_h202_mfg_upsample(input, count * 2u, output, &state);
    int rc = h2_pal_audio_track_write(track, &frame, 200u);
    if (rc != H2_PAL_OK) return (h2_pal_result_t)rc;
  }
  if (canceled != NULL && canceled(user)) return H2_PAL_ERR_CLOSED;
  return (h2_pal_result_t)h2_pal_audio_track_drain(track, 1000u);
}

h2_pal_result_t h2_h202_mfg_boot_feedback(h2_runtime_t* runtime,
                                          const h2_h202_mfg_profile_t* p) {
  if (runtime == NULL || p == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_led_color_t pixels[32];
  unsigned count = p->eye_count < 32u ? p->eye_count : 32u;
  for (unsigned i = 0; i < count; ++i)
    pixels[i] = p->colors[H2_H202_MFG_COLOR_WHITE];
  int light = h2_pal_led_set_frame(runtime->led, p->eyes, pixels, count);
  const h2_pal_audio_api_t* audio = runtime->audio;
  const h2_h202_mfg_adpcm_t* pcm = &p->prompts[H2_H202_MFG_PROMPT_BOOT];
  h2_pal_audio_track_t* track = NULL;
  bool speaker = false;
  int rc =
      pcm->data == NULL || pcm->size == NULL
          ? H2_PAL_ERR_NOT_FOUND
          : h2_pal_audio_set_speaker_volume_percent(audio, p->volume_percent);
  if (rc == H2_PAL_OK) {
    speaker = true; /* Partial start is still owned. */
    rc = h2_pal_audio_start_speaker(audio);
  }
  const h2_audio_track_config_t config = {
      .name = "h202/boot",
      .format = {.sample_rate_hz = p->sample_rate_hz,
                 .frame_samples_per_channel = p->frame_samples,
                 .channels = 1u,
                 .sample_format = H2_AUDIO_SAMPLE_S16LE},
      .volume_factor_milli = 1000u,
      .buffer_frames = 4u,
      .allocator = runtime->mem};
  if (rc == H2_PAL_OK) rc = h2_pal_audio_create_track(audio, &config, &track);
  if (rc == H2_PAL_OK)
    rc = h2_h202_mfg_play_adpcm8k(track, pcm->data, *pcm->size, NULL, NULL);
  if (track != NULL) {
    int closed = h2_pal_audio_track_close(track);
    if (rc == H2_PAL_OK) rc = closed;
  }
  if (speaker) {
    int stopped = h2_pal_audio_stop_speaker(audio);
    if (rc == H2_PAL_OK) rc = stopped;
  }
  char line[64];
  (void)snprintf(line, sizeof(line), "H202_MFG_BOOT_FEEDBACK eyes=%d prompt=%d",
                 light, rc);
  (void)h2_pal_log_write(runtime->log, H2_PAL_LOG_INFO, "h202/mfg", line);
  return (h2_pal_result_t)(light != H2_PAL_OK ? light : rc);
}

h2_pal_result_t h2_h202_mfg_scale_init(
    h2_h202_mfg_scale_t* scale, const h2_h202_mfg_scale_config_t* config) {
  if (scale == NULL || config == NULL || config->sample_rate_hz == 0u ||
      config->note_ms == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  for (unsigned i = 0; i < H2_H202_MFG_SCALE_NOTES; ++i)
    if (config->notes_hz[i] == 0u ||
        2u * config->notes_hz[i] >= config->sample_rate_hz)
      return H2_PAL_ERR_INVALID_ARG;
  memset(scale, 0, sizeof(*scale));
  scale->config = *config;
  uint64_t rate = config->sample_rate_hz;
  scale->note_samples = (uint32_t)(rate * config->note_ms / 1000u);
  scale->slot_samples =
      scale->note_samples + (uint32_t)(rate * config->gap_ms / 1000u);
  if (scale->note_samples == 0u) return H2_PAL_ERR_INVALID_ARG;
  /* 5 ms fades around gaps; a gapless melody keeps its phase instead. */
  scale->ramp_samples =
      config->gap_ms != 0u ? (uint32_t)(rate * 5u / 1000u) : 0u;
  if (scale->ramp_samples > scale->note_samples / 2u)
    scale->ramp_samples = scale->note_samples / 2u;
  return H2_PAL_OK;
}

int16_t h2_h202_mfg_scale_next(h2_h202_mfg_scale_t* scale) {
  float value = 0.0f;
  uint32_t at = scale->position;
  if (at < scale->note_samples) {
    uint32_t edge =
        at < scale->note_samples - 1u - at ? at : scale->note_samples - 1u - at;
    float gain = scale->ramp_samples != 0u && edge < scale->ramp_samples
                     ? (float)edge / (float)scale->ramp_samples
                     : 1.0f;
    value = (float)scale->config.amplitude * gain *
            sinf(2.0f * 3.14159265f * scale->phase);
    scale->phase += (float)scale->config.notes_hz[scale->note] /
                    (float)scale->config.sample_rate_hz;
    if (scale->phase >= 1.0f) scale->phase -= 1.0f;
  }
  if (++scale->position >= scale->slot_samples) {
    scale->position = 0u;
    if (scale->config.gap_ms != 0u) scale->phase = 0.0f;
    scale->note = (scale->note + 1u) % H2_H202_MFG_SCALE_NOTES;
  }
  return (int16_t)value;
}

/* ---- Service ---------------------------------------------------------- */

enum {
  MAILBOX = 16,
  TASK_STACK = 16384,
  WAIT_SLICE_MS = 20,
};

typedef struct slot {
  struct h2_h202_mfg_service* service;
  h2_pal_task_t* task;
  h2_pal_semaphore_t* wake;
  bool pending;
  bool busy;
  uint32_t wanted; /**< Live generation; any other value cancels the job. */
  h2_h202_mfg_request_t request;
  h2_h202_mfg_request_t job; /**< Worker-owned copy of the running job. */
} slot_t;

struct h2_h202_mfg_service {
  h2_runtime_t* runtime;
  const h2_h202_mfg_profile_t* profile;
  const h2_pal_sync_api_t* operation_sync;
  h2_pal_mutex_t* operation_mutex;
  bool drain_runtime_events;
  h2_pal_mutex_t* lock; /**< Slots, mailbox, status and stopping. */
  h2_pal_mutex_t* eyes; /**< Eye LED writes from controller and aging. */
  h2_pal_semaphore_t* events;
  h2_pal_task_t* controller_task;
  bool stopping;
  slot_t slots[H2_H202_MFG_OP_COUNT];
  h2_h202_mfg_completion_t mailbox[MAILBOX];
  unsigned mailbox_head;
  unsigned mailbox_count;
  h2_h202_mfg_status_t status;
  h2_h202_mfg_restore_t restore;
  uint32_t aging_volume_percent; /**< Controller request, protected by lock. */
  /* Controller task only. */
  h2_h202_mfg_t controller;
  unsigned raw_keys;
  unsigned stable_keys;
  uint64_t raw_since;
  h2_h202_mfg_charge_t charge;
  bool white_known;
  uint16_t white_duty_x100; /**< Last successfully applied PWM duty. */
  int charge_read_error;
  int charge_write_error;
  int logged_inputs; /**< Last logged USB/CHRG/DONE bits, -1 before any. */
  int logged_mode;   /**< Last logged charge indicator mode, -1 before any. */
  /* AUDIO worker only; observer callbacks run on that same worker. */
  h2_pal_audio_track_t* track;
  bool speaker;
  bool microphone;
  bool observer;
  int observer_error;
  h2_h202_mfg_aec_meter_t meter;
  bool meter_active;
  int16_t capture[640];
  int16_t output[320];
  h2_h202_mfg_scale_t scale;
};

static void service_lock(h2_h202_mfg_service_t* s) {
  (void)h2_pal_mutex_lock(s->runtime->sync, s->lock);
}

static void service_unlock(h2_h202_mfg_service_t* s) {
  (void)h2_pal_mutex_unlock(s->runtime->sync, s->lock);
}

static uint64_t service_now(h2_h202_mfg_service_t* s) {
  uint64_t now = 0;
  (void)h2_pal_time_get_monotonic_ms(s->runtime->time, &now);
  return now;
}

static void service_log(void* user, const char* line) {
  h2_h202_mfg_service_t* s = user;
  (void)h2_pal_log_write(s->runtime->log, H2_PAL_LOG_INFO, "h202/mfg", line);
}

static void slog(h2_h202_mfg_service_t* s, const char* format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  (void)vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  service_log(s, line);
}

static bool slot_canceled(void* user) {
  slot_t* slot = user;
  service_lock(slot->service);
  bool canceled =
      slot->service->stopping || slot->wanted != slot->job.generation;
  service_unlock(slot->service);
  return canceled;
}

/* Sleep in short slices; returns CLOSED once the job is canceled. */
static int slot_sleep(slot_t* slot, uint32_t ms) {
  while (!slot_canceled(slot)) {
    uint32_t part = ms > WAIT_SLICE_MS ? WAIT_SLICE_MS : ms;
    if (part == 0u) return H2_PAL_OK;
    int rc = h2_pal_time_sleep_ms(slot->service->runtime->time, part);
    if (rc != H2_PAL_OK) return rc;
    ms -= part;
  }
  return H2_PAL_ERR_CLOSED;
}

static void post(slot_t* slot, h2_h202_mfg_event_t event, int result,
                 int32_t value) {
  h2_h202_mfg_service_t* s = slot->service;
  const h2_h202_mfg_completion_t completion = {
      .operation = slot->job.operation,
      .generation = slot->job.generation,
      .event = event,
      .result = (h2_pal_result_t)result,
      .value = value};
  service_lock(s);
  while (s->mailbox_count == MAILBOX && !s->stopping) {
    service_unlock(s);
    (void)h2_pal_time_sleep_ms(s->runtime->time, 10u);
    service_lock(s);
  }
  if (!s->stopping) {
    s->mailbox[(s->mailbox_head + s->mailbox_count) % MAILBOX] = completion;
    ++s->mailbox_count;
  }
  service_unlock(s);
  (void)h2_pal_semaphore_give(s->runtime->sync, s->events);
}

/* Stage-sensitive work shares the BK App command lock with management. Lock
 * and unlock always run on the same worker task. */
static int operation_lock(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  while (!slot_canceled(slot)) {
    int rc = h2_pal_mutex_try_lock(s->operation_sync, s->operation_mutex);
    if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_BUSY) return rc;
    rc = slot_sleep(slot, WAIT_SLICE_MS);
    if (rc != H2_PAL_OK) return rc;
  }
  return H2_PAL_ERR_CLOSED;
}

static void operation_unlock(h2_h202_mfg_service_t* s) {
  (void)h2_pal_mutex_unlock(s->operation_sync, s->operation_mutex);
}

/* ---- Eyes and charge indication --------------------------------------- */

/* Stable names in H202_MFG_* serial events, consumed by the test monitor. */
static const char* const s_color_names[H2_H202_MFG_COLOR_COUNT] = {
    "off",        "amber", "purple", "cyan", "blue", "blue_left",
    "blue_right", "green", "yellow", "red",  "white"};
static const char* const s_prompt_names[H2_H202_MFG_PROMPT_COUNT] = {
    "charge_insert", "charge_remove",   "charge_passed", "nfc",
    "nfc_passed",    "wifi_passed",     "mic_left",      "mic_right",
    "listen",        "power",           "plus",          "minus",
    "aging",         "waiting",         "charge_failed", "nfc_failed",
    "wifi_failed",   "mic_failed",      "aging_failed",  "incomplete",
    "unsaved",       "poweroff_failed", "network",       "boot",
    "retry",         "retry_poweroff",  "battery_failed"};
static const char* const s_mode_names[] = {"off", "on", "blink", "breathe"};

static int eyes_frame(h2_h202_mfg_service_t* s, h2_h202_mfg_color_t color) {
  const h2_h202_mfg_profile_t* p = s->profile;
  h2_pal_led_color_t pixels[32];
  unsigned count = p->eye_count < 32u ? p->eye_count : 32u;
  for (unsigned i = 0; i < count; ++i) {
    bool left = (p->left_eye_mask >> i) & 1u;
    bool lit = color == H2_H202_MFG_COLOR_BLUE_LEFT    ? left
               : color == H2_H202_MFG_COLOR_BLUE_RIGHT ? !left
                                                       : true;
    pixels[i] = lit ? p->colors[color] : (h2_pal_led_color_t){0};
  }
  return h2_pal_led_set_frame(s->runtime->led, p->eyes, pixels, count);
}

static h2_pal_result_t service_light(void* user, h2_h202_mfg_color_t color) {
  h2_h202_mfg_service_t* s = user;
  if ((unsigned)color >= H2_H202_MFG_COLOR_COUNT) return H2_PAL_ERR_INVALID_ARG;
  (void)h2_pal_mutex_lock(s->runtime->sync, s->eyes);
  int rc = eyes_frame(s, color);
  (void)h2_pal_mutex_unlock(s->runtime->sync, s->eyes);
  slog(s, "H202_MFG_EYES color=%s rc=%d", s_color_names[color], rc);
  return (h2_pal_result_t)rc;
}

static int read_switch(h2_h202_mfg_service_t* s, h2_pal_periph_id_t id,
                       bool* on) {
  h2_pal_switch_state_t state = H2_PAL_SWITCH_STATE_OFF;
  int rc = h2_pal_switch_get(s->runtime->switch_api, id, &state);
  *on = state == H2_PAL_SWITCH_STATE_ON;
  return rc;
}

/* Background for the whole session; never a result, errors logged only. */
static void charge_tick(h2_h202_mfg_service_t* s, uint64_t now) {
  bool usb = false, charging = false, done = false;
  uint16_t duty_x100 = 0u;
  int rc = read_switch(s, s->profile->usb_insert, &usb);
  if (rc == H2_PAL_OK) rc = read_switch(s, s->profile->charging, &charging);
  if (rc == H2_PAL_OK) rc = read_switch(s, s->profile->charge_done, &done);
  if (rc != H2_PAL_OK) {
    if (rc != s->charge_read_error) slog(s, "H202_MFG_CHARGE read rc=%d", rc);
    s->charge_read_error = rc;
    return;
  }
  s->charge_read_error = H2_PAL_OK;
  int inputs = (usb ? 1 : 0) | (charging ? 2 : 0) | (done ? 4 : 0);
  if (inputs != s->logged_inputs) {
    slog(s, "H202_MFG_CHARGER usb=%d chrg=%d done=%d", usb, charging, done);
    s->logged_inputs = inputs;
  }
  h2_h202_mfg_charge_mode_t mode = h2_h202_mfg_charge_update(
      &s->charge, now, usb, charging, done, &duty_x100);
  if ((int)mode != s->logged_mode) {
    slog(s, "H202_MFG_WHITE mode=%s", s_mode_names[mode]);
    s->logged_mode = (int)mode;
  }
  if (s->white_known && duty_x100 == s->white_duty_x100) return;
  rc = h2_pal_pwm_switch_set_duty(s->runtime->pwm_switch, s->profile->white,
                                  duty_x100);
  if (rc != s->charge_write_error)
    slog(s, "H202_MFG_CHARGE white pwm rc=%d", rc);
  s->charge_write_error = rc;
  if (rc == H2_PAL_OK) {
    s->white_known = true;
    s->white_duty_x100 = duty_x100;
  } else {
    s->white_known = false;
  }
}

static unsigned read_keys_raw(h2_h202_mfg_service_t* s) {
  unsigned keys = 0u;
  for (unsigned i = 0; i < 4u; ++i) {
    h2_pal_single_button_reading_t reading;
    memset(&reading, 0, sizeof(reading));
    if (h2_pal_button_read_single_button(
            s->runtime->button, s->profile->keys[i], &reading) == H2_PAL_OK &&
        reading.state == H2_PAL_BUTTON_STATE_PRESSED)
      keys |= 1u << i;
  }
  return keys;
}

static unsigned read_keys(h2_h202_mfg_service_t* s, uint64_t now) {
  unsigned raw = read_keys_raw(s);
  if (raw != s->raw_keys) {
    s->raw_keys = raw;
    s->raw_since = now;
  } else if (now - s->raw_since >= s->profile->key_debounce_ms &&
             raw != s->stable_keys) {
    /* Bits follow profile keys: 1 power, 2 Record, 4 Vol+, 8 Vol-. */
    slog(s, "H202_MFG_KEYS mask=%u", raw);
    s->stable_keys = raw;
  }
  return s->stable_keys;
}

/* ---- Audio jobs ------------------------------------------------------- */

static void observe_capture(void* user, const h2_audio_aec_frame_t* frame) {
  h2_h202_mfg_service_t* s = user;
  if (s->meter_active) h2_h202_mfg_aec_meter_capture(&s->meter, frame);
}

static void observe_playback(void* user, const h2_audio_frame_t* frame) {
  (void)user;
  (void)frame;
}

static void observe_error(void* user, int capture, int result) {
  h2_h202_mfg_service_t* s = user;
  (void)capture;
  if (s->observer_error == H2_PAL_OK)
    s->observer_error = result != H2_PAL_OK ? result : H2_PAL_ERR_IO;
}

/* Releases what is still held; a failed step keeps its handle for retry. */
static int audio_release(h2_h202_mfg_service_t* s) {
  const h2_pal_audio_api_t* audio = s->runtime->audio;
  int rc = H2_PAL_OK;
  if (s->track != NULL) {
    rc = h2_pal_audio_track_close(s->track);
    if (rc != H2_PAL_OK) return rc;
    s->track = NULL;
  }
  if (s->microphone) {
    rc = h2_pal_audio_stop_mic(audio);
    if (rc != H2_PAL_OK) return rc;
    s->microphone = false;
  }
  if (s->speaker) {
    rc = h2_pal_audio_stop_speaker(audio);
    if (rc != H2_PAL_OK) return rc;
    s->speaker = false;
  }
  if (s->observer) {
    rc = h2_pal_audio_set_aec_observer(audio, NULL);
    if (rc != H2_PAL_OK) return rc;
    s->observer = false;
  }
  return H2_PAL_OK;
}

static int audio_close(h2_h202_mfg_service_t* s) {
  int rc = audio_release(s);
  if (rc == H2_PAL_OK) s->meter_active = false;
  return rc;
}

/* The observer is registered while microphone and speaker are stopped. */
static int audio_open(h2_h202_mfg_service_t* s, bool microphone,
                      const h2_audio_aec_observer_t* observer) {
  const h2_pal_audio_api_t* audio = s->runtime->audio;
  const h2_h202_mfg_profile_t* p = s->profile;
  /* A failed close keeps its handles; finish that cleanup first instead of
   * dropping a track the mixer is still closing. */
  int rc = audio_release(s);
  if (rc != H2_PAL_OK) return rc;
  if (observer != NULL) {
    s->observer_error = H2_PAL_OK;
    rc = h2_pal_audio_set_aec_observer(audio, observer);
    s->observer = rc == H2_PAL_OK;
  }
  if (rc == H2_PAL_OK)
    rc = h2_pal_audio_set_speaker_volume_percent(audio, p->volume_percent);
  if (rc == H2_PAL_OK) {
    s->speaker = true; /* Partial start is still owned. */
    rc = h2_pal_audio_start_speaker(audio);
  }
  const h2_audio_track_config_t track = {
      .name = "h202/mfg",
      .format = {.sample_rate_hz = p->sample_rate_hz,
                 .frame_samples_per_channel = p->frame_samples,
                 .channels = 1u,
                 .sample_format = H2_AUDIO_SAMPLE_S16LE},
      .volume_factor_milli = 1000u,
      .buffer_frames = 4u,
      .allocator = s->runtime->mem};
  if (rc == H2_PAL_OK) rc = h2_pal_audio_create_track(audio, &track, &s->track);
  if (rc == H2_PAL_OK && microphone) {
    s->microphone = true;
    rc = h2_pal_audio_start_mic(audio);
  }
  return rc;
}

/* Retry instructions are stored once and spoken after each failure. */
static h2_h202_mfg_prompt_t prompt_tail(h2_h202_mfg_prompt_t prompt) {
  switch (prompt) {
    case H2_H202_MFG_PROMPT_CHARGE_FAILED:
    case H2_H202_MFG_PROMPT_NFC_FAILED:
    case H2_H202_MFG_PROMPT_WIFI_FAILED:
    case H2_H202_MFG_PROMPT_MIC_FAILED:
    case H2_H202_MFG_PROMPT_AGING_FAILED:
    case H2_H202_MFG_PROMPT_BATTERY_FAILED:
      return H2_H202_MFG_PROMPT_RETRY;
    case H2_H202_MFG_PROMPT_UNSAVED:
    case H2_H202_MFG_PROMPT_POWEROFF_FAILED:
      return H2_H202_MFG_PROMPT_RETRY_POWEROFF;
    default:
      return H2_H202_MFG_PROMPT_NONE;
  }
}

static int play_clip(slot_t* slot, h2_h202_mfg_prompt_t prompt) {
  h2_h202_mfg_service_t* s = slot->service;
  if ((unsigned)prompt >= H2_H202_MFG_PROMPT_COUNT)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_h202_mfg_adpcm_t* clip = &s->profile->prompts[prompt];
  slog(s, "H202_MFG_PLAY prompt=%s", s_prompt_names[prompt]);
  return clip->data == NULL || clip->size == NULL
             ? H2_PAL_ERR_NOT_FOUND
             : h2_h202_mfg_play_adpcm8k(s->track, clip->data, *clip->size,
                                        slot_canceled, slot);
}

static int play_prompts(slot_t* slot, const h2_h202_mfg_prompt_t* prompts,
                        unsigned count) {
  h2_h202_mfg_service_t* s = slot->service;
  int rc = audio_open(s, false, NULL);
  for (unsigned i = 0; rc == H2_PAL_OK && i < count; ++i) {
    rc = play_clip(slot, prompts[i]);
    const h2_h202_mfg_prompt_t tail = prompt_tail(prompts[i]);
    if (rc == H2_PAL_OK && tail != H2_H202_MFG_PROMPT_NONE)
      rc = play_clip(slot, tail);
  }
  int closed = audio_close(s);
  return rc == H2_PAL_OK ? closed : rc;
}

/* Rainbow on every eye pixel; the eye lock and cancellation check keep a
 * late frame from overwriting the controller's stage 10 color. */
static int rainbow(slot_t* slot, uint64_t elapsed_ms) {
  h2_h202_mfg_service_t* s = slot->service;
  const h2_h202_mfg_profile_t* p = s->profile;
  h2_pal_led_color_t pixels[32];
  unsigned count = p->eye_count < 32u ? p->eye_count : 32u;
  for (unsigned i = 0; i < count; ++i) {
    unsigned phase =
        (unsigned)((elapsed_ms / 10u + i * 256u / (count ? count : 1u)) % 255u);
    unsigned segment = phase / 85u, x = (phase % 85u) * 3u;
    uint8_t up = (uint8_t)x, down = (uint8_t)(255u - x);
    pixels[i] = segment == 0u   ? (h2_pal_led_color_t){down, up, 0, 0}
                : segment == 1u ? (h2_pal_led_color_t){0, down, up, 0}
                                : (h2_pal_led_color_t){up, 0, down, 0};
  }
  (void)h2_pal_mutex_lock(s->runtime->sync, s->eyes);
  int rc = slot_canceled(slot)
               ? H2_PAL_OK
               : h2_pal_led_set_frame(s->runtime->led, p->eyes, pixels, count);
  (void)h2_pal_mutex_unlock(s->runtime->sync, s->eyes);
  return rc;
}

/* Stage 05: one capture frame in, that side's AEC output out at a fixed low
 * gain. */
static int audio_cycle(slot_t* slot, unsigned lane) {
  h2_h202_mfg_service_t* s = slot->service;
  const h2_h202_mfg_profile_t* p = s->profile;
  h2_audio_frame_t capture = {.data = s->capture,
                              .capacity = sizeof(s->capture)};
  int rc = h2_pal_audio_mic_read(s->runtime->audio, &capture, 100u);
  if (rc != H2_PAL_OK) return rc;
  if (capture.channels != 2u ||
      capture.samples_per_channel != p->frame_samples ||
      capture.sample_format != H2_AUDIO_SAMPLE_S16LE ||
      capture.bytes < (size_t)p->frame_samples * 4u)
    return H2_PAL_ERR_IO;
  for (unsigned i = 0; i < p->frame_samples; ++i) {
    int32_t sample =
        ((int32_t)s->capture[i * 2u + lane] * p->monitor_gain_q15) >> 15;
    s->output[i] = (int16_t)(sample > INT16_MAX   ? INT16_MAX
                             : sample < INT16_MIN ? INT16_MIN
                                                  : sample);
  }
  const h2_audio_pcm_format_t format = {
      .sample_rate_hz = p->sample_rate_hz,
      .frame_samples_per_channel = p->frame_samples,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE};
  h2_audio_frame_t playback =
      h2_audio_frame_for_buffer(s->output, sizeof(s->output), format);
  playback.bytes = (size_t)p->frame_samples * sizeof(int16_t);
  rc = h2_pal_audio_track_write(s->track, &playback, 100u);
  if (rc == H2_PAL_OK && s->observer_error != H2_PAL_OK) rc = s->observer_error;
  return rc;
}

static void run_mic(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  const h2_h202_mfg_profile_t* p = s->profile;
  uint8_t lane = slot->job.item == H2_H202_MFG_RIGHT_MIC ? 1u : 0u;
  int rc = play_prompts(slot, slot->job.prompts, slot->job.prompt_count);
  const h2_audio_aec_observer_t observer = {.user = s,
                                            .on_capture = observe_capture,
                                            .on_playback = observe_playback,
                                            .on_error = observe_error};
  if (rc == H2_PAL_OK)
    rc = h2_h202_mfg_aec_meter_init(&s->meter, &p->aec, lane);
  if (rc == H2_PAL_OK) {
    s->meter_active = true;
    rc = audio_open(s, true, &observer);
    slog(s, "H202_MFG_TONE lane=%s on rc=%d", lane ? "right" : "left", rc);
  }
  /* The operator judges the loopback; the side runs for its whole window
   * and levels are only logged. A capture fault restarts the audio rather
   * than ending the window; only a restart that fails is an error. */
  const uint64_t limit =
      service_now(s) + p->aec.convergence_ms + p->aec.measure_ms;
  unsigned restarts = 0u;
  while (rc == H2_PAL_OK && service_now(s) < limit) {
    if (slot_canceled(slot)) {
      rc = H2_PAL_ERR_CLOSED;
      break;
    }
    rc = audio_cycle(slot, lane);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      rc = H2_PAL_OK;
    if (rc != H2_PAL_OK && !slot_canceled(slot) && restarts < 3u) {
      ++restarts;
      slog(s, "H202_MFG_MIC_RESTART lane=%u rc=%d", lane, rc);
      rc = audio_close(s);
      if (rc == H2_PAL_OK) {
        s->meter_active = true;
        rc = audio_open(s, true, &observer);
      }
    }
  }
  s->meter_active = false;
  int closed = audio_close(s);
  slog(s, "H202_MFG_TONE off rc=%d", closed);
  if (rc == H2_PAL_OK) rc = closed;
  h2_h202_mfg_aec_result_t result;
  h2_h202_mfg_aec_verdict_t verdict =
      h2_h202_mfg_aec_meter_result(&s->meter, &result);
  slog(s,
       "H202_MFG_AEC lane=%u rc=%d verdict=%d blocks=%u ref=%ld raw=%ld "
       "processed=%ld",
       lane, rc, verdict, result.blocks, (long)result.reference_dbfs_x10,
       (long)result.raw_dbfs_x10, (long)result.processed_dbfs_x10);
  post(slot, H2_H202_MFG_EVENT_DONE, rc, verdict);
}

/* Eye rainbow for aging on the sensor worker, until canceled. */
static void run_rainbow(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  uint64_t started = service_now(s);
  int rc = rainbow(slot, 0u);
  slog(s, "H202_MFG_EYES color=rainbow rc=%d", rc);
  while (rc == H2_PAL_OK && !slot_canceled(slot)) {
    rc = slot_sleep(slot, s->profile->aging_frame_ms);
    if (rc == H2_PAL_OK) rc = rainbow(slot, service_now(s) - started);
  }
  if (rc == H2_PAL_ERR_CLOSED && slot_canceled(slot)) rc = H2_PAL_OK;
  post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
}

/* One melody frame to the speaker; the track write paces the loop. */
static int aging_cycle(h2_h202_mfg_service_t* s) {
  const h2_h202_mfg_profile_t* p = s->profile;
  for (unsigned i = 0; i < p->frame_samples; ++i)
    s->output[i] = h2_h202_mfg_scale_next(&s->scale);
  const h2_audio_pcm_format_t format = {
      .sample_rate_hz = p->sample_rate_hz,
      .frame_samples_per_channel = p->frame_samples,
      .channels = 1u,
      .sample_format = H2_AUDIO_SAMPLE_S16LE};
  h2_audio_frame_t playback =
      h2_audio_frame_for_buffer(s->output, sizeof(s->output), format);
  playback.bytes = (size_t)p->frame_samples * sizeof(int16_t);
  return h2_pal_audio_track_write(s->track, &playback, 100u);
}

/* Playback only, like the H106 Zero BK aging: no microphone or AEC, so a
 * capture queue can never overflow here. Only an audio error fails aging;
 * the rainbow worker reports RGB errors. A requested stop that cleans up
 * without error is the PASS evidence. */
static void run_aging(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  const h2_h202_mfg_profile_t* p = s->profile;
  int rc = play_prompts(slot, slot->job.prompts, slot->job.prompt_count);
  if (rc == H2_PAL_OK) rc = h2_h202_mfg_scale_init(&s->scale, &p->aging_scale);
  if (rc == H2_PAL_OK) {
    rc = audio_open(s, false, NULL);
    slog(s, "H202_MFG_TONE melody on rc=%d", rc);
  }
  uint32_t volume = p->volume_percent;
  bool first = true;
  /* Report frames written every 5 s; about 250 means real time. */
  uint64_t window = service_now(s);
  unsigned cycles = 0u;
  while (rc == H2_PAL_OK && !slot_canceled(slot)) {
    service_lock(s);
    uint32_t requested = s->aging_volume_percent;
    service_unlock(s);
    if (first || requested != volume) {
      rc =
          h2_pal_audio_set_speaker_volume_percent(s->runtime->audio, requested);
      slog(s, "H202_MFG_AGING_VOLUME percent=%lu rc=%d",
           (unsigned long)requested, rc);
      if (rc != H2_PAL_OK) break;
      volume = requested;
    }
    first = false;
    rc = aging_cycle(s);
    if (rc == H2_PAL_OK) ++cycles;
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_TIMEOUT)
      rc = H2_PAL_OK;
    uint64_t now = service_now(s);
    if (now - window >= 5000u) {
      slog(s, "H202_MFG_AGING_LOOP frames=%u", cycles);
      window = now;
      cycles = 0u;
    }
  }
  if (rc == H2_PAL_ERR_CLOSED && slot_canceled(slot)) rc = H2_PAL_OK;
  int closed = audio_close(s);
  slog(s, "H202_MFG_TONE off rc=%d", closed);
  if (rc == H2_PAL_OK) rc = closed;
  slog(s, "H202_MFG_AGING rc=%d", rc);
  post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
}

/* ---- Sensor jobs ------------------------------------------------------ */

static void run_charger(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  h2_pal_battery_reading_t battery;
  memset(&battery, 0, sizeof(battery));
  int read = h2_pal_input_read_battery(s->runtime->input, s->profile->battery,
                                       &battery);
  bool present =
      read == H2_PAL_OK && (battery.flags & H2_PAL_BATTERY_PRESENT) != 0u;
  slog(s, "H202_MFG_BATTERY rc=%d present=%d mv=%ld", read, present ? 1 : 0,
       (long)battery.voltage_mv);
  post(slot, H2_H202_MFG_EVENT_BATTERY, read, present ? 1 : 0);
  bool stable = false, candidate = false;
  int rc = read_switch(s, s->profile->usb_insert, &stable);
  if (rc == H2_PAL_OK) post(slot, H2_H202_MFG_EVENT_CHARGER, rc, stable);
  candidate = stable;
  uint64_t since = service_now(s);
  while (rc == H2_PAL_OK) {
    rc = slot_sleep(slot, s->profile->tick_ms);
    bool current = false;
    if (rc == H2_PAL_OK) rc = read_switch(s, s->profile->usb_insert, &current);
    if (rc != H2_PAL_OK) break;
    uint64_t now = service_now(s);
    if (current != candidate) {
      candidate = current;
      since = now;
    } else if (candidate != stable &&
               now - since >= s->profile->charger_debounce_ms) {
      stable = candidate;
      post(slot, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, stable);
    }
  }
  post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
}

static void run_nfc(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  int rc = H2_PAL_OK, last = H2_PAL_OK;
  for (;;) {
    h2_pal_nfc_scan_t scan;
    memset(&scan, 0, sizeof(scan));
    rc = h2_pal_nfc_scan_nfc_reader(s->runtime->nfc, s->profile->nfc, &scan);
    if (rc == H2_PAL_ERR_UNSUPPORTED || rc == H2_PAL_ERR_NOT_FOUND) break;
    if (rc == H2_PAL_OK && scan.stage == H2_PAL_NFC_STAGE_ERROR)
      rc = scan.result != H2_PAL_OK ? scan.result : H2_PAL_ERR_IO;
    /* The FM175xx reader only discovers ISO14443A tags; any UID passes. */
    if (rc == H2_PAL_OK && scan.stage == H2_PAL_NFC_STAGE_DISCOVERED &&
        scan.uid_len >= 4u && scan.uid_len <= sizeof(scan.uid)) {
      char uid[2 * sizeof(scan.uid) + 1];
      for (unsigned i = 0; i < scan.uid_len; ++i)
        (void)snprintf(uid + i * 2u, 3, "%02x", scan.uid[i]);
      slog(s, "H202_MFG_NFC uid=%s type=%d", uid, scan.tag_type);
      break;
    }
    if (rc != H2_PAL_OK && rc != last) slog(s, "H202_MFG_NFC scan rc=%d", rc);
    last = rc;
    rc = slot_sleep(slot, 100u);
    if (rc != H2_PAL_OK) break;
  }
  post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
}

/* ---- Network jobs ----------------------------------------------------- */

typedef struct ap_selection {
  h2_pal_wifi_sta_config_t* config;
  int best_rssi;
} ap_selection_t;

static bool select_ap(void* user, const h2_pal_wifi_scan_entry_t* entry) {
  ap_selection_t* selection = user;
  h2_pal_wifi_sta_config_t* config = selection->config;
  if (entry->ssid_len == config->ssid_len &&
      entry->rssi > selection->best_rssi &&
      memcmp(entry->ssid, config->ssid, config->ssid_len) == 0) {
    memcpy(config->bssid, entry->bssid, sizeof(config->bssid));
    config->bssid_set = 1u;
    config->channel = entry->channel;
    selection->best_rssi = entry->rssi;
  }
  return true;
}

static bool wifi_up(h2_h202_mfg_service_t* s) {
  h2_pal_wifi_sta_status_t status;
  memset(&status, 0, sizeof(status));
  size_t length = strlen(s->profile->wifi_ssid);
  return h2_pal_wifi_sta_get_status(s->runtime->wifi_sta, &status) ==
             H2_PAL_OK &&
         h2_pal_wifi_sta_status_has_ip(&status) && status.ip_valid &&
         status.ip.ip4 != 0u && status.ssid_len == length &&
         memcmp(status.ssid, s->profile->wifi_ssid, length) == 0;
}

static int wifi_time(slot_t* slot, const h2_pal_netif_ref_t* ref) {
  h2_h202_mfg_service_t* s = slot->service;
  h2_runtime_t* runtime = s->runtime;
  h2_pal_net_resolver_t* resolver = NULL;
  h2_pal_net_addr_t server;
  memset(&server, 0, sizeof(server));
  int rc =
      h2_pal_net_resolve_start(runtime->net, s->profile->ntp_host, &resolver);
  if (rc != H2_PAL_OK) return rc;
  uint64_t started = service_now(s);
  do {
    rc = h2_pal_net_resolve_poll(runtime->net, resolver, &server, 100u);
    if (rc != H2_PAL_ERR_TIMEOUT && rc != H2_PAL_ERR_WOULD_BLOCK) break;
    if (slot_canceled(slot)) rc = H2_PAL_ERR_CLOSED;
  } while (rc != H2_PAL_ERR_CLOSED && service_now(s) - started < 5000u);
  h2_pal_net_resolve_close(runtime->net, resolver);
  if (rc != H2_PAL_OK) return rc;
  server.port = H2_NTP_PORT;
  const h2_pal_net_bind_t bind = {.type = H2_PAL_NET_BIND_NETIF, .netif = ref};
  const h2_ntp_client_config_t ntp = {.net = runtime->net,
                                      .time = runtime->time,
                                      .server = server,
                                      .bind = &bind,
                                      .timeout_ms = 2000u,
                                      .retries = 2u,
                                      .set_wall_clock = 1u};
  h2_ntp_sync_result_t result;
  memset(&result, 0, sizeof(result));
  rc = h2_ntp_sync(&ntp, &result);
  return rc == H2_NTP_OK && result.wall_clock_set ? H2_PAL_OK : H2_PAL_ERR_IO;
}

/* Fixture association, usable IPv4 and time sync through Wi-Fi. Scan and
 * association change routes and share BK state with management Wi-Fi
 * commands, which take the same operation mutex, so qualification holds it
 * except while waiting for DHCP and rechecks the link after. */
static int wifi_attempt(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  h2_runtime_t* runtime = s->runtime;
  h2_pal_wifi_sta_config_t config;
  memset(&config, 0, sizeof(config));
  size_t ssid = strlen(s->profile->wifi_ssid);
  size_t password = strlen(s->profile->wifi_password);
  if (ssid == 0u || ssid > H2_PAL_WIFI_SSID_MAX ||
      password > H2_PAL_WIFI_PASSWORD_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  memcpy(config.ssid, s->profile->wifi_ssid, ssid);
  config.ssid_len = ssid;
  memcpy(config.password, s->profile->wifi_password, password);
  config.password_len = password;
  h2_pal_wifi_scan_request_t scan;
  memset(&scan, 0, sizeof(scan));
  memcpy(scan.ssid, config.ssid, ssid);
  scan.ssid_len = ssid;
  ap_selection_t selection = {.config = &config, .best_rssi = -1000};
  int rc = operation_lock(slot);
  if (rc != H2_PAL_OK) return rc;
  (void)h2_pal_wifi_sta_disconnect(runtime->wifi_sta);
  (void)h2_pal_wifi_sta_scan(runtime->wifi_sta, &scan, select_ap, &selection,
                             5000u);
  rc = slot_canceled(slot)
           ? H2_PAL_ERR_CLOSED
           : h2_pal_wifi_sta_connect(runtime->wifi_sta, &config, 15000u);
  /* Connect returns once associated; DHCP finishes later. Wait without the
   * operation mutex so management is not held off; the
   * controller's Wi-Fi budget cancels this slot if no address arrives. */
  operation_unlock(s);
  while (rc == H2_PAL_OK && !wifi_up(s)) rc = slot_sleep(slot, 200u);
  if (rc == H2_PAL_OK) rc = operation_lock(slot);
  if (rc != H2_PAL_OK) return slot_canceled(slot) ? H2_PAL_ERR_CLOSED : rc;
  /* Management may have changed the link while the lock was released. */
  if (!wifi_up(s)) rc = H2_PAL_ERR_INVALID_STATE;
  h2_pal_netif_ref_t ref;
  memset(&ref, 0, sizeof(ref));
  const h2_pal_netif_filter_t filter = {.kind = H2_PAL_NETIF_KIND_WIFI_STA};
  if (rc == H2_PAL_OK) rc = h2_pal_netif_find(runtime->netif, &filter, &ref);
  if (rc == H2_PAL_OK) rc = wifi_time(slot, &ref);
  operation_unlock(s);
  return slot_canceled(slot) ? H2_PAL_ERR_CLOSED : rc;
}

static void run_network(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  if (slot->job.job == H2_H202_MFG_JOB_CHECK) {
    int rc = wifi_attempt(slot);
    slog(s, "H202_MFG_NETWORK op=wifi rc=%d", rc);
    post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
    return;
  }
  /* Aging: keep the stage 04 link and reconnect only after it drops; the
   * disconnect, scan and association disturb live audio capture. Losses are
   * logged only. */
  while (!slot_canceled(slot)) {
    int rc = H2_PAL_OK;
    if (wifi_up(s)) {
      slog(s, "H202_MFG_NETWORK op=wifi maintain=connected");
    } else {
      rc = wifi_attempt(slot);
      slog(s, "H202_MFG_NETWORK op=wifi reconnect=%d", rc);
    }
    while (rc == H2_PAL_OK && slot_sleep(slot, 1000u) == H2_PAL_OK &&
           wifi_up(s)) {
    }
    if (!slot_canceled(slot)) slog(s, "H202_MFG_NETWORK op=wifi disconnected");
    (void)slot_sleep(slot, 1000u);
  }
  post(slot, H2_H202_MFG_EVENT_DONE, H2_PAL_ERR_CLOSED, 0);
}

/* ---- Shutdown --------------------------------------------------------- */

/* Commit happens only here. A management reboot never reaches this job. */
/* Every other job was canceled; wait for them to release audio/radios. */
static void wait_jobs_released(h2_h202_mfg_service_t* s) {
  for (unsigned waited = 0; waited < 5000u; waited += WAIT_SLICE_MS) {
    bool busy = false;
    service_lock(s);
    for (unsigned op = 0; op < H2_H202_MFG_OP_POWER; ++op)
      busy = busy || s->slots[op].busy;
    bool stopping = s->stopping;
    service_unlock(s);
    if (!busy || stopping) break;
    (void)h2_pal_time_sleep_ms(s->runtime->time, WAIT_SLICE_MS);
  }
}

static void run_shutdown(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  int rc = h2_pal_mutex_lock(s->operation_sync, s->operation_mutex);
  if (rc != H2_PAL_OK) {
    post(slot, H2_H202_MFG_EVENT_COMMITTED, rc, 0);
    return;
  }
  wait_jobs_released(s);
  rc = h2_h202_mfg_progress_commit(s->runtime->pref, slot->job.results);
  slog(s, "H202_MFG_COMMIT rc=%d", rc);
  post(slot, H2_H202_MFG_EVENT_COMMITTED, rc, 0);
  if (rc != H2_PAL_OK) {
    operation_unlock(s);
    return;
  }
  rc = h2_pal_power_shutdown(s->runtime->power, 0u);
  if (rc == H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE; /* Did not power off. */
  slog(s, "H202_MFG_POWEROFF rc=%d", rc);
  operation_unlock(s);
  post(slot, H2_H202_MFG_EVENT_POWER_ERROR, rc, 0);
}

/* Save only; the worker keeps waiting for the next firmware. */
static void run_accept(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  int rc = h2_pal_mutex_lock(s->operation_sync, s->operation_mutex);
  if (rc == H2_PAL_OK) {
    rc = h2_h202_mfg_progress_commit(s->runtime->pref, slot->job.results);
    operation_unlock(s);
  }
  slog(s, "H202_MFG_ACCEPT rc=%d", rc);
  post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
}

/* ---- Tasks ------------------------------------------------------------ */

/* Clear under the operation mutex, then reboot; returning means no reboot. */
static void run_reset(slot_t* slot) {
  h2_h202_mfg_service_t* s = slot->service;
  int rc = h2_pal_mutex_lock(s->operation_sync, s->operation_mutex);
  if (rc == H2_PAL_OK) {
    wait_jobs_released(s);
    rc = h2_h202_mfg_progress_clear(s->runtime->pref);
    slog(s, "H202_MFG_RESET clear rc=%d", rc);
    if (rc == H2_PAL_OK) {
      rc = h2_pal_power_reboot(s->runtime->power, 0u);
      if (rc == H2_PAL_OK) rc = H2_PAL_ERR_INVALID_STATE;
      slog(s, "H202_MFG_REBOOT rc=%d", rc);
    }
    operation_unlock(s);
  }
  post(slot, H2_H202_MFG_EVENT_POWER_ERROR, rc, 0);
}

static void run_job(slot_t* slot) {
  switch (slot->job.job) {
    case H2_H202_MFG_JOB_PROMPT: {
      int rc = play_prompts(slot, slot->job.prompts, slot->job.prompt_count);
      post(slot, H2_H202_MFG_EVENT_DONE, rc, 0);
      break;
    }
    case H2_H202_MFG_JOB_MIC:
      run_mic(slot);
      break;
    case H2_H202_MFG_JOB_AGING:
      run_aging(slot);
      break;
    case H2_H202_MFG_JOB_CHARGER:
      run_charger(slot);
      break;
    case H2_H202_MFG_JOB_NFC:
      run_nfc(slot);
      break;
    case H2_H202_MFG_JOB_RAINBOW:
      run_rainbow(slot);
      break;
    case H2_H202_MFG_JOB_CHECK:
    case H2_H202_MFG_JOB_MAINTAIN:
      run_network(slot);
      break;
    case H2_H202_MFG_JOB_SHUTDOWN:
      run_shutdown(slot);
      break;
    case H2_H202_MFG_JOB_RESET:
      run_reset(slot);
      break;
    case H2_H202_MFG_JOB_ACCEPT:
      run_accept(slot);
      break;
  }
}

static void worker(void* user) {
  slot_t* slot = user;
  h2_h202_mfg_service_t* s = slot->service;
  for (;;) {
    (void)h2_pal_semaphore_take(s->runtime->sync, slot->wake, 100u);
    service_lock(s);
    bool stopping = s->stopping, have = slot->pending;
    if (have && !stopping) {
      slot->job = slot->request;
      slot->pending = false;
      slot->busy = true;
    }
    service_unlock(s);
    if (stopping) break;
    if (!have) continue;
    run_job(slot);
    service_lock(s);
    slot->busy = false;
    service_unlock(s);
  }
}

static h2_pal_result_t service_begin(void* user,
                                     const h2_h202_mfg_request_t* request) {
  h2_h202_mfg_service_t* s = user;
  if ((unsigned)request->operation >= H2_H202_MFG_OP_COUNT)
    return H2_PAL_ERR_INVALID_ARG;
  slot_t* slot = &s->slots[request->operation];
  service_lock(s);
  bool stopping = s->stopping;
  if (!stopping) {
    slot->request = *request;
    slot->pending = true;
    slot->wanted = request->generation;
    if (request->job == H2_H202_MFG_JOB_AGING)
      s->aging_volume_percent = s->profile->volume_percent;
  }
  service_unlock(s);
  if (stopping) return H2_PAL_ERR_CLOSED;
  (void)h2_pal_semaphore_give(s->runtime->sync, slot->wake);
  return H2_PAL_OK;
}

/* A pending job is kept: it starts, sees the cancel and still reports. */
static void service_cancel(void* user, h2_h202_mfg_operation_t operation,
                           uint32_t generation) {
  h2_h202_mfg_service_t* s = user;
  if ((unsigned)operation >= H2_H202_MFG_OP_COUNT) return;
  service_lock(s);
  if (s->slots[operation].wanted == generation) s->slots[operation].wanted = 0u;
  service_unlock(s);
}

static void service_aging_volume(void* user, int delta_percent) {
  h2_h202_mfg_service_t* s = user;
  service_lock(s);
  int volume = (int)s->aging_volume_percent + delta_percent;
  s->aging_volume_percent = volume < 0     ? 0u
                            : volume > 100 ? 100u
                                           : (uint32_t)volume;
  service_unlock(s);
}

static void drain_runtime(h2_h202_mfg_service_t* s) {
  h2_runtime_event_payload_buffer_t payload;
  for (unsigned i = 0; i < 32u; ++i) {
    h2_runtime_event_t event = {.payload = payload.bytes,
                                .payload_capacity = sizeof(payload.bytes)};
    if (h2_runtime_poll_event(s->runtime, &event) != H2_PAL_OK) break;
  }
}

static void controller_task(void* user) {
  h2_h202_mfg_service_t* s = user;
  uint64_t now = service_now(s);
  s->raw_keys = s->stable_keys = read_keys_raw(s);
  s->raw_since = now;
  charge_tick(s, now);
  (void)h2_h202_mfg_start(&s->controller, &s->restore, now, s->stable_keys);
  service_lock(s);
  (void)h2_h202_mfg_status(&s->controller, &s->status);
  service_unlock(s);
  for (;;) {
    h2_h202_mfg_completion_t completions[MAILBOX];
    size_t count = 0;
    service_lock(s);
    bool stopping = s->stopping;
    while (s->mailbox_count != 0u) {
      completions[count++] = s->mailbox[s->mailbox_head];
      s->mailbox_head = (s->mailbox_head + 1u) % MAILBOX;
      --s->mailbox_count;
    }
    service_unlock(s);
    if (stopping) break;
    now = service_now(s);
    unsigned keys = read_keys(s, now);
    charge_tick(s, now);
    (void)h2_h202_mfg_update(&s->controller, now, keys, completions, count);
    service_lock(s);
    (void)h2_h202_mfg_status(&s->controller, &s->status);
    service_unlock(s);
    if (s->drain_runtime_events) drain_runtime(s);
    (void)h2_pal_semaphore_take(s->runtime->sync, s->events,
                                s->profile->tick_ms);
  }
}

static const char* const s_task_names[H2_H202_MFG_OP_COUNT] = {
    "h202/mfg/audio", "h202/mfg/sensor", "h202/mfg/wifi", "h202/mfg/power"};

static void service_free(h2_h202_mfg_service_t* s) {
  const h2_pal_sync_api_t* sync = s->runtime->sync;
  for (unsigned op = 0; op < H2_H202_MFG_OP_COUNT; ++op)
    if (s->slots[op].wake != NULL)
      (void)h2_pal_semaphore_destroy(sync, s->slots[op].wake);
  if (s->events != NULL) (void)h2_pal_semaphore_destroy(sync, s->events);
  if (s->eyes != NULL) (void)h2_pal_mutex_destroy(sync, s->eyes);
  if (s->lock != NULL) (void)h2_pal_mutex_destroy(sync, s->lock);
  h2_pal_mem_free(s->runtime->mem, s);
}

static h2_pal_result_t service_join(h2_h202_mfg_service_t* s) {
  service_lock(s);
  s->stopping = true;
  for (unsigned op = 0; op < H2_H202_MFG_OP_COUNT; ++op)
    s->slots[op].wanted = 0u;
  service_unlock(s);
  h2_pal_result_t result = H2_PAL_OK;
  if (s->controller_task != NULL) {
    (void)h2_pal_semaphore_give(s->runtime->sync, s->events);
    int rc = h2_pal_task_join(s->runtime->task, s->controller_task);
    if (rc == H2_PAL_OK)
      s->controller_task = NULL;
    else
      result = (h2_pal_result_t)rc;
  }
  for (unsigned op = 0; op < H2_H202_MFG_OP_COUNT; ++op) {
    slot_t* slot = &s->slots[op];
    if (slot->task == NULL) continue;
    (void)h2_pal_semaphore_give(s->runtime->sync, slot->wake);
    int rc = h2_pal_task_join(s->runtime->task, slot->task);
    if (rc == H2_PAL_OK)
      slot->task = NULL;
    else
      result = (h2_pal_result_t)rc;
  }
  return result;
}

h2_pal_result_t h2_h202_mfg_service_start(
    const h2_h202_mfg_service_config_t* config, h2_h202_mfg_service_t** out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (config == NULL || config->runtime == NULL || config->profile == NULL ||
      config->operation_sync == NULL || config->operation_mutex == NULL ||
      config->profile->tick_ms == 0u || config->profile->eye_count == 0u ||
      config->profile->frame_samples == 0u ||
      config->profile->frame_samples > 320u ||
      config->profile->wifi_ssid == NULL ||
      config->profile->wifi_password == NULL ||
      config->profile->ntp_host == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_runtime_t* runtime = config->runtime;
  h2_h202_mfg_service_t* s = h2_pal_mem_alloc(runtime->mem, sizeof(*s));
  if (s == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->logged_inputs = -1;
  s->logged_mode = -1;
  s->runtime = runtime;
  s->profile = config->profile;
  s->operation_sync = config->operation_sync;
  s->operation_mutex = config->operation_mutex;
  s->drain_runtime_events = config->drain_runtime_events;
  h2_h202_mfg_charge_init(
      &s->charge, s->profile->charge_window_ms, s->profile->charge_toggles,
      s->profile->charge_blink_period_ms, s->profile->charge_breathe_period_ms);
  h2_h202_mfg_config_t controller = s->profile->controller;
  controller.callbacks =
      (h2_h202_mfg_callbacks_t){.user = s,
                                .begin = service_begin,
                                .cancel = service_cancel,
                                .light = service_light,
                                .aging_volume = service_aging_volume,
                                .log = service_log};
  const h2_pal_mutex_config_t mutex = {.name = "h202/mfg",
                                       .allocator = runtime->mem};
  const h2_pal_semaphore_config_t wake = {
      .name = "h202/mfg", .allocator = runtime->mem, .max_count = 1u};
  int rc = h2_h202_mfg_init(&s->controller, &controller);
  if (rc == H2_PAL_OK)
    rc = h2_pal_mutex_create(runtime->sync, &mutex, &s->lock);
  if (rc == H2_PAL_OK)
    rc = h2_pal_mutex_create(runtime->sync, &mutex, &s->eyes);
  if (rc == H2_PAL_OK)
    rc = h2_pal_semaphore_create(runtime->sync, &wake, &s->events);
  for (unsigned op = 0; rc == H2_PAL_OK && op < H2_H202_MFG_OP_COUNT; ++op) {
    s->slots[op].service = s;
    rc = h2_pal_semaphore_create(runtime->sync, &wake, &s->slots[op].wake);
  }
  if (rc != H2_PAL_OK) {
    service_free(s);
    return (h2_pal_result_t)rc;
  }
  /* One-shot progress before any stage runs, on this task under the lock. */
  rc = h2_pal_mutex_lock(s->operation_sync, s->operation_mutex);
  if (rc == H2_PAL_OK) {
    h2_h202_mfg_progress_load(runtime->pref, runtime->mem, &s->restore);
    operation_unlock(s);
  } else {
    s->restore.error = (h2_pal_result_t)rc;
  }
  /* results: one digit per item, 0 untested 1 passed 2 skipped 3 failed. */
  char saved[H2_H202_MFG_ITEM_COUNT + 1];
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    saved[i] = (char)('0' + s->restore.results[i] % 10u);
  saved[H2_H202_MFG_ITEM_COUNT] = '\0';
  slog(s, "H202_MFG_PROGRESS present=%d rc=%d results=%s",
       s->restore.present ? 1 : 0, s->restore.error, saved);
  rc = H2_PAL_OK; /* A progress failure starts at stage 02, never blocks. */
  for (unsigned op = 0; rc == H2_PAL_OK && op < H2_H202_MFG_OP_COUNT; ++op) {
    const h2_pal_task_options_t options = {.name = s_task_names[op],
                                           .min_stack_size = TASK_STACK};
    rc = h2_pal_task_start(runtime->task, &options, worker, &s->slots[op],
                           &s->slots[op].task);
  }
  if (rc == H2_PAL_OK) {
    const h2_pal_task_options_t options = {.name = "h202/mfg",
                                           .min_stack_size = TASK_STACK};
    rc = h2_pal_task_start(runtime->task, &options, controller_task, s,
                           &s->controller_task);
  }
  if (rc != H2_PAL_OK) {
    if (service_join(s) == H2_PAL_OK) service_free(s);
    return (h2_pal_result_t)rc;
  }
  *out = s;
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_mfg_service_status(h2_h202_mfg_service_t* service,
                                           h2_h202_mfg_status_t* out) {
  if (service == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  service_lock(service);
  *out = service->status;
  service_unlock(service);
  return H2_PAL_OK;
}

h2_pal_result_t h2_h202_mfg_service_stop(h2_h202_mfg_service_t** service) {
  if (service == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (*service == NULL) return H2_PAL_OK;
  h2_h202_mfg_service_t* s = *service;
  h2_pal_result_t rc = service_join(s);
  if (rc != H2_PAL_OK) return rc;
  (void)audio_close(s);
  service_free(s);
  *service = NULL;
  return H2_PAL_OK;
}
