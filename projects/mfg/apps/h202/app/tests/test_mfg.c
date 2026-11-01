#include "h2_h202_mfg.h"
#include "h2_loader_boot.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Synchronous controller fixture: begin/cancel/light are recorded and the
 * test plays the workers by feeding completions. */
typedef struct fake {
  h2_h202_mfg_request_t requests[256];
  unsigned count;
  uint32_t live[H2_H202_MFG_OP_COUNT];
  uint32_t canceled[256];
  unsigned cancels;
  h2_h202_mfg_color_t color;
  h2_pal_result_t begin_rc;
} fake_t;

static fake_t f;
static h2_h202_mfg_t app;
static uint64_t now;
static unsigned keys;

static h2_pal_result_t fake_begin(void* user,
                                  const h2_h202_mfg_request_t* request) {
  (void)user;
  assert(request->generation != 0u);
  if (f.begin_rc != H2_PAL_OK) return f.begin_rc;
  assert(f.count < 256u);
  f.requests[f.count++] = *request;
  f.live[request->operation] = request->generation;
  return H2_PAL_OK;
}

static void fake_cancel(void* user, h2_h202_mfg_operation_t op,
                        uint32_t generation) {
  (void)user;
  (void)op;
  assert(f.cancels < 256u);
  f.canceled[f.cancels++] = generation;
}

static h2_pal_result_t fake_light(void* user, h2_h202_mfg_color_t color) {
  (void)user;
  f.color = color;
  return H2_PAL_OK;
}

static const h2_h202_mfg_config_t config = {
    .charger_timeout_ms = 60000u,
    .nfc_timeout_ms = 60000u,
    .wifi_timeout_ms = 30000u,
    .cellular_timeout_ms = 60000u,
    .power_hold_ms = 2000u,
    .callbacks = {
        .begin = fake_begin, .cancel = fake_cancel, .light = fake_light}};

static h2_h202_mfg_status_t status(void) {
  h2_h202_mfg_status_t out;
  assert(h2_h202_mfg_status(&app, &out) == H2_PAL_OK);
  return out;
}

static void step(uint64_t ms) {
  now += ms;
  assert(h2_h202_mfg_update(&app, now, keys, NULL, 0) == H2_PAL_OK);
}

static void set_keys(unsigned mask) {
  keys = mask;
  step(30);
}

static void click(unsigned key) {
  set_keys(keys | key);
  set_keys(keys & ~key);
}

static void event(h2_h202_mfg_operation_t op, h2_h202_mfg_event_t ev,
                  h2_pal_result_t rc, int32_t value) {
  const h2_h202_mfg_completion_t c = {.operation = op,
                                      .generation = f.live[op],
                                      .event = ev,
                                      .result = rc,
                                      .value = value};
  now += 5;
  assert(h2_h202_mfg_update(&app, now, keys, &c, 1) == H2_PAL_OK);
}

static void done(h2_h202_mfg_operation_t op, h2_pal_result_t rc,
                 int32_t value) {
  event(op, H2_H202_MFG_EVENT_DONE, rc, value);
}

static const h2_h202_mfg_request_t* last(h2_h202_mfg_operation_t op) {
  for (unsigned i = f.count; i > 0; --i)
    if (f.requests[i - 1].operation == op) return &f.requests[i - 1];
  return NULL;
}

static unsigned count_jobs(h2_h202_mfg_job_t job) {
  unsigned n = 0;
  for (unsigned i = 0; i < f.count; ++i) n += f.requests[i].job == job;
  return n;
}

static void begin_test(const h2_h202_mfg_restore_t* restore) {
  memset(&f, 0, sizeof(f));
  now = 1000;
  keys = 0;
  assert(h2_h202_mfg_init(&app, &config) == H2_PAL_OK);
  assert(h2_h202_mfg_start(&app, restore, now, 0) == H2_PAL_OK);
}

static void expect(h2_h202_mfg_item_t item, h2_h202_mfg_phase_t phase) {
  h2_h202_mfg_status_t s = status();
  if (s.item != item || s.phase != phase) {
    fprintf(stderr, "expected item=%d phase=%d, got item=%d phase=%d\n", item,
            phase, s.item, s.phase);
    abort();
  }
}

static void expect_prompt(h2_h202_mfg_prompt_t prompt) {
  const h2_h202_mfg_request_t* r = last(H2_H202_MFG_OP_AUDIO);
  assert(r != NULL && r->prompt_count >= 1u && r->prompts[0] == prompt);
}

/* Stage 02 through stage 04, with both networks passing. */
static void pass_to_mics(void) {
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  assert(f.color == H2_H202_MFG_COLOR_AMBER);
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 1);
  expect_prompt(H2_H202_MFG_PROMPT_CHARGE_REMOVE);
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 0);
  expect_prompt(H2_H202_MFG_PROMPT_CHARGE_INSERT);
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 1);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ANNOUNCE);
  expect_prompt(H2_H202_MFG_PROMPT_CHARGE_PASSED);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  assert(f.color == H2_H202_MFG_COLOR_PURPLE);
  expect_prompt(H2_H202_MFG_PROMPT_NFC);
  assert(last(H2_H202_MFG_OP_SENSOR)->job == H2_H202_MFG_JOB_NFC);
  /* 4G finishes in the background before stage 04. */
  done(H2_H202_MFG_OP_CELLULAR, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_SENSOR, H2_PAL_OK, 0);
  expect_prompt(H2_H202_MFG_PROMPT_NFC_PASSED);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_ACTIVE);
  assert(f.color == H2_H202_MFG_COLOR_CYAN);
  done(H2_H202_MFG_OP_WIFI, H2_PAL_OK, 0);
  expect_prompt(H2_H202_MFG_PROMPT_WIFI_PASSED);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_CELLULAR, H2_H202_MFG_PHASE_ANNOUNCE);
  expect_prompt(H2_H202_MFG_PROMPT_CELLULAR_PASSED);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_LEFT_MIC, H2_H202_MFG_PHASE_ACTIVE);
}

static void pass_mics_and_keys(void) {
  expect(H2_H202_MFG_LEFT_MIC, H2_H202_MFG_PHASE_ACTIVE);
  assert(f.color == H2_H202_MFG_COLOR_BLUE_LEFT);
  assert(last(H2_H202_MFG_OP_AUDIO)->job == H2_H202_MFG_JOB_MIC);
  expect_prompt(H2_H202_MFG_PROMPT_MIC_LEFT);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  expect(H2_H202_MFG_RIGHT_MIC, H2_H202_MFG_PHASE_ACTIVE);
  assert(f.color == H2_H202_MFG_COLOR_BLUE_RIGHT);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  expect_prompt(H2_H202_MFG_PROMPT_LISTEN);
  click(H2_H202_MFG_KEY_RECORD);
  expect(H2_H202_MFG_POWER, H2_H202_MFG_PHASE_ACTIVE);
  click(H2_H202_MFG_KEY_POWER);
  expect(H2_H202_MFG_PLUS, H2_H202_MFG_PHASE_ACTIVE);
  click(H2_H202_MFG_KEY_PLUS);
  expect(H2_H202_MFG_MINUS, H2_H202_MFG_PHASE_ACTIVE);
  click(H2_H202_MFG_KEY_MINUS);
}

static void test_flow(void) {
  begin_test(NULL);
  assert(status().wifi == H2_H202_MFG_NET_RUNNING);
  assert(status().cellular == H2_H202_MFG_NET_RUNNING);
  pass_to_mics();
  pass_mics_and_keys();
  expect(H2_H202_MFG_AGING, H2_H202_MFG_PHASE_ACTIVE);
  assert(last(H2_H202_MFG_OP_AUDIO)->job == H2_H202_MFG_JOB_AGING);
  assert(last(H2_H202_MFG_OP_WIFI)->job == H2_H202_MFG_JOB_MAINTAIN);
  assert(last(H2_H202_MFG_OP_CELLULAR)->job == H2_H202_MFG_JOB_MAINTAIN);
  /* Aging network loss is diagnostic only. */
  done(H2_H202_MFG_OP_WIFI, H2_PAL_ERR_IO, 0);
  assert(status().results[H2_H202_MFG_WIFI] == H2_H202_MFG_PASSED);
  uint32_t aging = f.live[H2_H202_MFG_OP_AUDIO];
  unsigned cancels = f.cancels;
  click(H2_H202_MFG_KEY_RECORD);
  expect(H2_H202_MFG_AGING, H2_H202_MFG_PHASE_STOPPING);
  assert(f.cancels == cancels + 1u && f.canceled[cancels] == aging);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_WAITING);
  assert(status().stage == 10u);
  assert(f.color == H2_H202_MFG_COLOR_GREEN);
  expect_prompt(H2_H202_MFG_PROMPT_WAITING);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(status().results[i] == H2_H202_MFG_PASSED);
  /* Stage 10 ignores the skip gesture. */
  set_keys(H2_H202_MFG_KEY_RECORD);
  click(H2_H202_MFG_KEY_PLUS);
  set_keys(0);
  expect(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_WAITING);
  puts("flow ok");
}

static void skip_current(void) {
  set_keys(keys | H2_H202_MFG_KEY_RECORD);
  set_keys(keys | H2_H202_MFG_KEY_PLUS);
  set_keys(keys & ~(unsigned)H2_H202_MFG_KEY_PLUS);
}

static void test_skip_and_admission(void) {
  begin_test(NULL);
  /* Skip stage 02; releasing Record afterwards is consumed. */
  skip_current();
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  set_keys(0);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  assert(status().results[H2_H202_MFG_CHARGER] == H2_H202_MFG_SKIPPED);
  done(H2_H202_MFG_OP_SENSOR, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_WIFI, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_CELLULAR, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_LEFT_MIC, H2_H202_MFG_PHASE_ACTIVE);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  /* Skipping the right mic must not pass the Record confirmation. */
  skip_current();
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  set_keys(0);
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  assert(status().results[H2_H202_MFG_RIGHT_MIC] == H2_H202_MFG_SKIPPED);
  click(H2_H202_MFG_KEY_RECORD);
  click(H2_H202_MFG_KEY_POWER);
  expect(H2_H202_MFG_PLUS, H2_H202_MFG_PHASE_ACTIVE);
  /* The skip gesture at stage 07 records SKIPPED, never PASSED. */
  skip_current();
  set_keys(0);
  assert(status().results[H2_H202_MFG_PLUS] == H2_H202_MFG_SKIPPED);
  expect(H2_H202_MFG_MINUS, H2_H202_MFG_PHASE_ACTIVE);
  /* Plus pressed before Record is still not a Volume + press. */
  set_keys(H2_H202_MFG_KEY_PLUS);
  set_keys(H2_H202_MFG_KEY_PLUS | H2_H202_MFG_KEY_RECORD);
  set_keys(H2_H202_MFG_KEY_RECORD);
  set_keys(0);
  expect(H2_H202_MFG_MINUS, H2_H202_MFG_PHASE_ACTIVE);
  click(H2_H202_MFG_KEY_MINUS);
  /* Not every earlier result passed: no aging, yellow and incomplete. */
  expect(H2_H202_MFG_ITEM_COUNT, H2_H202_MFG_PHASE_WAITING);
  assert(count_jobs(H2_H202_MFG_JOB_AGING) == 0u);
  assert(status().results[H2_H202_MFG_AGING] == H2_H202_MFG_UNTESTED);
  assert(f.color == H2_H202_MFG_COLOR_YELLOW);
  expect_prompt(H2_H202_MFG_PROMPT_INCOMPLETE);
  puts("skip and aging admission ok");
}

static void test_failures_and_retry(void) {
  begin_test(NULL);
  /* Charger: 60 s without both transitions fails with the red light. */
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 0);
  step(59000);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  step(1000);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_FAILED);
  assert(status().error == H2_PAL_ERR_TIMEOUT);
  assert(f.color == H2_H202_MFG_COLOR_RED);
  expect_prompt(H2_H202_MFG_PROMPT_CHARGE_FAILED);
  /* Wi-Fi's 30 s attempt has also expired in the background. */
  assert(status().wifi == H2_H202_MFG_NET_FAILED);
  /* Only a Record press retries; Minus does nothing. */
  click(H2_H202_MFG_KEY_MINUS);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_FAILED);
  uint32_t old = f.live[H2_H202_MFG_OP_SENSOR];
  click(H2_H202_MFG_KEY_RECORD);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  assert(f.live[H2_H202_MFG_OP_SENSOR] != old);
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 0);
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 1);
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  /* NFC reader error is a failure with the NFC prompt. */
  done(H2_H202_MFG_OP_SENSOR, H2_PAL_ERR_UNSUPPORTED, 0);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_FAILED);
  expect_prompt(H2_H202_MFG_PROMPT_NFC_FAILED);
  click(H2_H202_MFG_KEY_RECORD);
  step(60000);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_FAILED);
  assert(status().error == H2_PAL_ERR_TIMEOUT);
  skip_current();
  set_keys(0);
  /* Stage 04 announces the already-failed Wi-Fi check immediately. */
  expect(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_FAILED);
  expect_prompt(H2_H202_MFG_PROMPT_WIFI_FAILED);
  /* A retry starts a new attempt with its own 30 s budget. */
  uint32_t first = f.live[H2_H202_MFG_OP_WIFI];
  click(H2_H202_MFG_KEY_RECORD);
  assert(f.live[H2_H202_MFG_OP_WIFI] != first);
  assert(last(H2_H202_MFG_OP_WIFI)->job == H2_H202_MFG_JOB_CHECK);
  expect(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_ACTIVE);
  step(29900);
  expect(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_ACTIVE);
  step(100);
  expect(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_FAILED);
  /* A late result of the expired attempt is stale. */
  h2_h202_mfg_completion_t late = {.operation = H2_H202_MFG_OP_WIFI,
                                   .generation = f.live[H2_H202_MFG_OP_WIFI],
                                   .event = H2_H202_MFG_EVENT_DONE};
  assert(h2_h202_mfg_update(&app, now, keys, &late, 1) == H2_PAL_OK);
  expect(H2_H202_MFG_WIFI, H2_H202_MFG_PHASE_FAILED);
  click(H2_H202_MFG_KEY_RECORD);
  done(H2_H202_MFG_OP_WIFI, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  /* 4G's 60 s budget ran out long ago. */
  expect(H2_H202_MFG_CELLULAR, H2_H202_MFG_PHASE_FAILED);
  expect_prompt(H2_H202_MFG_PROMPT_CELLULAR_FAILED);
  skip_current();
  set_keys(0);
  /* Microphone measurement failure and an instruction prompt error. */
  expect(H2_H202_MFG_LEFT_MIC, H2_H202_MFG_PHASE_ACTIVE);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_REDUCTION_LOW);
  expect(H2_H202_MFG_LEFT_MIC, H2_H202_MFG_PHASE_FAILED);
  expect_prompt(H2_H202_MFG_PROMPT_MIC_FAILED);
  click(H2_H202_MFG_KEY_RECORD);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_ERR_IO, 0);
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_FAILED);
  assert(status().results[H2_H202_MFG_LISTEN] == H2_H202_MFG_FAILED);
  puts("failures, timeouts and retry ok");
}

static void test_stale_and_new_presses(void) {
  begin_test(NULL);
  uint32_t charger = f.live[H2_H202_MFG_OP_SENSOR];
  event(H2_H202_MFG_OP_SENSOR, H2_H202_MFG_EVENT_CHARGER, H2_PAL_OK, 0);
  skip_current();
  set_keys(0);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  /* The canceled charger job reporting late must not affect NFC. */
  h2_h202_mfg_completion_t stale[2] = {{.operation = H2_H202_MFG_OP_SENSOR,
                                        .generation = charger,
                                        .event = H2_H202_MFG_EVENT_CHARGER,
                                        .value = 1},
                                       {.operation = H2_H202_MFG_OP_SENSOR,
                                        .generation = charger,
                                        .event = H2_H202_MFG_EVENT_DONE,
                                        .result = H2_PAL_ERR_IO}};
  assert(h2_h202_mfg_update(&app, now, keys, stale, 2) == H2_PAL_OK);
  expect(H2_H202_MFG_NFC, H2_H202_MFG_PHASE_ACTIVE);
  assert(status().results[H2_H202_MFG_NFC] == H2_H202_MFG_UNTESTED);
  done(H2_H202_MFG_OP_SENSOR, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_WIFI, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_CELLULAR, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, 0);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  /* Record pressed during the right mic, released after LISTEN starts. */
  set_keys(H2_H202_MFG_KEY_RECORD);
  done(H2_H202_MFG_OP_AUDIO, H2_PAL_OK, H2_H202_MFG_AEC_PASS);
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  set_keys(0);
  expect(H2_H202_MFG_LISTEN, H2_H202_MFG_PHASE_ACTIVE);
  click(H2_H202_MFG_KEY_RECORD);
  expect(H2_H202_MFG_POWER, H2_H202_MFG_PHASE_ACTIVE);
  /* Plus already held when stage 07 starts is not a new press. */
  set_keys(H2_H202_MFG_KEY_PLUS);
  click(H2_H202_MFG_KEY_POWER);
  expect(H2_H202_MFG_PLUS, H2_H202_MFG_PHASE_ACTIVE);
  set_keys(0);
  expect(H2_H202_MFG_PLUS, H2_H202_MFG_PHASE_ACTIVE);
  click(H2_H202_MFG_KEY_PLUS);
  expect(H2_H202_MFG_MINUS, H2_H202_MFG_PHASE_ACTIVE);
  /* Backwards time and unknown keys are rejected without mutation. */
  assert(h2_h202_mfg_update(&app, now - 1, keys, NULL, 0) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_h202_mfg_update(&app, now, 16u, NULL, 0) == H2_PAL_ERR_INVALID_ARG);
  expect(H2_H202_MFG_MINUS, H2_H202_MFG_PHASE_ACTIVE);
  puts("stale events and new presses ok");
}

static void test_power_hold(void) {
  begin_test(NULL);
  /* A short power press in stage 02 neither passes nor powers off. */
  click(H2_H202_MFG_KEY_POWER);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  set_keys(H2_H202_MFG_KEY_POWER);
  step(1900);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  step(100);
  assert(status().phase == H2_H202_MFG_PHASE_SAVING);
  const h2_h202_mfg_request_t* shutdown = last(H2_H202_MFG_OP_POWER);
  assert(shutdown != NULL && shutdown->job == H2_H202_MFG_JOB_SHUTDOWN);
  /* Commit failure: unsaved prompt, Record retries. */
  event(H2_H202_MFG_OP_POWER, H2_H202_MFG_EVENT_COMMITTED, H2_PAL_ERR_IO, 0);
  assert(status().phase == H2_H202_MFG_PHASE_UNSAVED);
  expect_prompt(H2_H202_MFG_PROMPT_UNSAVED);
  set_keys(0);
  click(H2_H202_MFG_KEY_RECORD);
  assert(status().phase == H2_H202_MFG_PHASE_SAVING);
  event(H2_H202_MFG_OP_POWER, H2_H202_MFG_EVENT_COMMITTED, H2_PAL_OK, 0);
  assert(status().committed);
  event(H2_H202_MFG_OP_POWER, H2_H202_MFG_EVENT_POWER_ERROR, H2_PAL_ERR_IO, 0);
  assert(status().phase == H2_H202_MFG_PHASE_POWEROFF_FAILED);
  expect_prompt(H2_H202_MFG_PROMPT_POWEROFF_FAILED);
  assert(f.color == H2_H202_MFG_COLOR_RED);
  click(H2_H202_MFG_KEY_RECORD);
  assert(status().phase == H2_H202_MFG_PHASE_SAVING);
  /* A 2 s hold inside stage 06 shuts down instead of passing it. */
  begin_test(NULL);
  skip_current();
  skip_current();
  skip_current();
  skip_current();
  skip_current();
  skip_current();
  skip_current();
  set_keys(0);
  expect(H2_H202_MFG_POWER, H2_H202_MFG_PHASE_ACTIVE);
  set_keys(H2_H202_MFG_KEY_POWER);
  step(2500);
  set_keys(0);
  assert(status().phase == H2_H202_MFG_PHASE_SAVING);
  assert(status().results[H2_H202_MFG_POWER] == H2_H202_MFG_UNTESTED);
  puts("power hold ok");
}

/* ---- One-shot progress through h2_loader_mfg_* ----------------------- */

typedef struct entry {
  char key[32];
  uint8_t blob[64];
  size_t len;
  bool is_u32;
  uint32_t u32;
  bool used;
} entry_t;

static entry_t store[8];
static int fail_set_blob_after = -1;
static h2_pal_pref_namespace_t ns;

static entry_t* find(const char* key, bool create) {
  for (unsigned i = 0; i < 8; ++i)
    if (store[i].used && strcmp(store[i].key, key) == 0) return &store[i];
  if (!create) return NULL;
  for (unsigned i = 0; i < 8; ++i)
    if (!store[i].used) {
      memset(&store[i], 0, sizeof(store[i]));
      store[i].used = true;
      snprintf(store[i].key, sizeof(store[i].key), "%s", key);
      return &store[i];
    }
  abort();
}

static int ns_close(h2_pal_pref_namespace_t* n) {
  (void)n;
  return H2_PAL_OK;
}
static int ns_commit(h2_pal_pref_namespace_t* n) {
  (void)n;
  return H2_PAL_OK;
}
static int ns_get_blob(h2_pal_pref_namespace_t* n, const h2_pal_mem_api_t* mem,
                       const char* key, void** out, size_t* len) {
  (void)n;
  entry_t* e = find(key, false);
  if (e == NULL || e->is_u32) return H2_PAL_ERR_NOT_FOUND;
  *out = h2_pal_mem_alloc(mem, e->len);
  if (*out == NULL) return H2_PAL_ERR_NO_MEMORY;
  memcpy(*out, e->blob, e->len);
  *len = e->len;
  return H2_PAL_OK;
}
static int ns_set_blob(h2_pal_pref_namespace_t* n, const char* key,
                       const void* data, size_t len) {
  (void)n;
  if (fail_set_blob_after == 0) return H2_PAL_ERR_IO;
  if (fail_set_blob_after > 0) --fail_set_blob_after;
  assert(len <= 64u);
  entry_t* e = find(key, true);
  e->is_u32 = false;
  memcpy(e->blob, data, len);
  e->len = len;
  return H2_PAL_OK;
}
static int ns_get_u32(h2_pal_pref_namespace_t* n, const char* key,
                      uint32_t* out) {
  (void)n;
  entry_t* e = find(key, false);
  if (e == NULL || !e->is_u32) return H2_PAL_ERR_NOT_FOUND;
  *out = e->u32;
  return H2_PAL_OK;
}
static int ns_set_u32(h2_pal_pref_namespace_t* n, const char* key,
                      uint32_t value) {
  (void)n;
  entry_t* e = find(key, true);
  e->is_u32 = true;
  e->u32 = value;
  return H2_PAL_OK;
}
static int pref_open(void* user, const char* space,
                     h2_pal_pref_open_mode_t mode,
                     h2_pal_pref_namespace_t** out) {
  (void)user;
  (void)mode;
  assert(strcmp(space, "h2loader") == 0);
  ns = (h2_pal_pref_namespace_t){.close = ns_close,
                                 .get_blob = ns_get_blob,
                                 .set_blob = ns_set_blob,
                                 .get_u32 = ns_get_u32,
                                 .set_u32 = ns_set_u32,
                                 .commit = ns_commit};
  *out = &ns;
  return H2_PAL_OK;
}
static const h2_pal_pref_vtable_t pref_vtable = {.open = pref_open};
static const h2_pal_pref_api_t pref = {.vtable = &pref_vtable};

static void* mem_alloc(void* user, size_t n) {
  (void)user;
  return malloc(n);
}
static void mem_free(void* user, void* p) {
  (void)user;
  free(p);
}
static const h2_pal_mem_vtable_t mem_vtable = {.alloc = mem_alloc,
                                               .free = mem_free};
static const h2_pal_mem_api_t mem = {.vtable = &mem_vtable};

static void stored(h2_loader_mfg_summary_t* out) {
  int present = 0;
  assert(h2_loader_mfg_read(&pref, &mem, out, &present) == H2_PAL_OK);
  assert(present);
}

static void test_progress(void) {
  memset(store, 0, sizeof(store));
  h2_h202_mfg_restore_t restore;
  /* No record: revision binding creates an empty one; nothing restored. */
  h2_h202_mfg_progress_load(&pref, &mem, &restore);
  assert(restore.error == H2_PAL_OK);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(restore.results[i] == H2_H202_MFG_UNTESTED);
  h2_loader_mfg_summary_t summary;
  stored(&summary);
  assert(summary.total == H2_H202_MFG_ITEM_COUNT);
  /* Commit, then the next boot reads, clears and resumes. */
  uint8_t results[H2_H202_MFG_ITEM_COUNT] = {
      H2_H202_MFG_PASSED, H2_H202_MFG_PASSED, H2_H202_MFG_PASSED,
      H2_H202_MFG_SKIPPED, H2_H202_MFG_PASSED};
  assert(h2_h202_mfg_progress_commit(&pref, results) == H2_PAL_OK);
  h2_h202_mfg_progress_load(&pref, &mem, &restore);
  assert(restore.error == H2_PAL_OK && restore.present);
  assert(memcmp(restore.results, results, sizeof(results)) == 0);
  stored(&summary);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(summary.step_status[i] == H2_LOADER_MFG_STEP_UNTESTED);
  /* Resume: first item not PASSED is 4G; only 4G runs in the background. */
  begin_test(&restore);
  expect(H2_H202_MFG_CELLULAR, H2_H202_MFG_PHASE_ACTIVE);
  assert(status().wifi == H2_H202_MFG_NET_IDLE);
  assert(status().cellular == H2_H202_MFG_NET_RUNNING);
  assert(count_jobs(H2_H202_MFG_JOB_CHECK) == 1u);
  /* Later PASSED results are kept and their items are not rerun. */
  assert(status().results[H2_H202_MFG_LEFT_MIC] == H2_H202_MFG_PASSED);
  assert(status().results[H2_H202_MFG_CELLULAR] == H2_H202_MFG_UNTESTED);
  /* A reset after resume finds the cleared record: stage 02. */
  h2_h202_mfg_progress_load(&pref, &mem, &restore);
  begin_test(&restore);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  /* Failed clear: discard the restored results and report. */
  assert(h2_h202_mfg_progress_commit(&pref, results) == H2_PAL_OK);
  fail_set_blob_after = 0;
  h2_h202_mfg_progress_load(&pref, &mem, &restore);
  fail_set_blob_after = -1;
  assert(restore.error == H2_PAL_ERR_IO && !restore.present);
  begin_test(&restore);
  expect(H2_H202_MFG_CHARGER, H2_H202_MFG_PHASE_ACTIVE);
  assert(status().restore_error == H2_PAL_ERR_IO);
  /* Incompatible revision or step count: reset, nothing restored. */
  assert(h2_h202_mfg_progress_commit(&pref, results) == H2_PAL_OK);
  find("mfg_acceptance_revision", false)->u32 = H2_H202_MFG_REVISION + 1u;
  h2_h202_mfg_progress_load(&pref, &mem, &restore);
  assert(restore.error == H2_PAL_OK);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(restore.results[i] == H2_H202_MFG_UNTESTED);
  h2_loader_mfg_summary_t legacy = {.total = 22u};
  legacy.step_status[0] = H2_LOADER_MFG_STEP_PASSED;
  assert(h2_loader_mfg_write(&pref, &legacy) == H2_PAL_OK);
  h2_h202_mfg_progress_load(&pref, &mem, &restore);
  assert(restore.error == H2_PAL_OK);
  for (unsigned i = 0; i < H2_H202_MFG_ITEM_COUNT; ++i)
    assert(restore.results[i] == H2_H202_MFG_UNTESTED);
  stored(&summary);
  assert(summary.total == H2_H202_MFG_ITEM_COUNT);
  /* Invalid statuses are never written. */
  uint8_t bad[H2_H202_MFG_ITEM_COUNT] = {4};
  assert(h2_h202_mfg_progress_commit(&pref, bad) == H2_PAL_ERR_INVALID_ARG);
  puts("one-shot progress ok");
}

/* ---- Charge indication ------------------------------------------------ */

static h2_h202_mfg_charge_mode_t charge(h2_h202_mfg_charge_t* c, uint64_t t,
                                        bool usb, bool chrg, bool done,
                                        bool* led) {
  return h2_h202_mfg_charge_update(c, t, usb, chrg, done, led);
}

static void test_charge(void) {
  h2_h202_mfg_charge_t c;
  bool led = true;
  h2_h202_mfg_charge_init(&c, 2000u, 2u, 500u);
  assert(charge(&c, 0, false, false, false, &led) == H2_H202_MFG_CHARGE_OFF);
  assert(!led);
  assert(charge(&c, 100, true, true, false, &led) == H2_H202_MFG_CHARGE_ON);
  assert(led);
  /* Charging ends in full: one CHRG change, the indicator turns off. */
  assert(charge(&c, 5000, true, false, true, &led) == H2_H202_MFG_CHARGE_OFF);
  assert(!led);
  /* USB in, both status lines off: undervoltage/temperature fault blinks. */
  h2_h202_mfg_charge_init(&c, 2000u, 2u, 500u);
  assert(charge(&c, 1000, true, false, false, &led) ==
         H2_H202_MFG_CHARGE_BLINK);
  assert(led);
  (void)charge(&c, 1250, true, false, false, &led);
  assert(!led);
  (void)charge(&c, 1500, true, false, false, &led);
  assert(led);
  /* No battery: CHRG toggling with DONE on blinks; one change is full. */
  h2_h202_mfg_charge_init(&c, 2000u, 2u, 500u);
  assert(charge(&c, 0, true, false, true, &led) == H2_H202_MFG_CHARGE_OFF);
  assert(charge(&c, 500, true, true, true, &led) == H2_H202_MFG_CHARGE_BLINK);
  assert(charge(&c, 1000, true, false, true, &led) == H2_H202_MFG_CHARGE_BLINK);
  assert(charge(&c, 2400, true, false, true, &led) == H2_H202_MFG_CHARGE_BLINK);
  assert(charge(&c, 2600, true, false, true, &led) == H2_H202_MFG_CHARGE_OFF);
  /* Toggles spread wider than the window are not blinking. */
  assert(charge(&c, 5000, true, true, true, &led) == H2_H202_MFG_CHARGE_BLINK);
  assert(charge(&c, 7500, true, false, true, &led) == H2_H202_MFG_CHARGE_OFF);
  /* Unplugged always turns the indicator off. */
  assert(charge(&c, 7600, false, true, true, &led) == H2_H202_MFG_CHARGE_OFF);
  assert(!led);
  puts("charge indication ok");
}

int main(void) {
  test_flow();
  test_skip_and_admission();
  test_failures_and_retry();
  test_stale_and_new_presses();
  test_power_hold();
  test_progress();
  test_charge();
  return 0;
}
