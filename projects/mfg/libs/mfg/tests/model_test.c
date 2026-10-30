#include "h2_mfg_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const h2_mfg_step_t steps[] = {
    {"storage", H2_MFG_STORAGE, 1},
    {"display", H2_MFG_DISPLAY, 0},
    {"touch", H2_MFG_TOUCH, 0},
};
static const h2_mfg_profile_t profile = {
    .board = "test", .revision = 1, .steps = steps, .total = 3};
static h2_loader_mfg_summary_t durable;
static int save_result;
static unsigned writes;

static int persist(void* user, const h2_loader_mfg_summary_t* summary) {
  assert(user == &durable);
  ++writes;
  if (save_result == 0) durable = *summary;
  return save_result;
}

static h2_mfg_model_t fresh(void) {
  h2_mfg_model_t m;
  memset(&durable, 0, sizeof(durable));
  durable.total = 3;
  save_result = 0;
  writes = 0;
  assert(h2_mfg_model_init(&m, &profile, &durable, 0x11223344, persist,
                           &durable) == 0);
  return m;
}

static void confirmation_and_resume(void) {
  h2_mfg_model_t m = fresh();
  assert(h2_mfg_model_action(&m, "confirm", "storage", m.ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  m.ready = 1;
  assert(h2_mfg_model_action(&m, "confirm", "display", m.ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_mfg_model_action(&m, "confirm", "storage", m.ticket - 1) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(writes == 0);
  uint32_t old_ticket = m.ticket;
  assert(h2_mfg_model_action(&m, "confirm", "storage", m.ticket) == 0);
  assert(m.current == 1 && !m.ready && durable.step_status[0] == 1);
  m.ready = 1;
  assert(h2_mfg_model_action(&m, "confirm", "display", old_ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_mfg_model_t resumed;
  assert(h2_mfg_model_init(&resumed, &profile, &durable, 0x55667788, persist,
                           &durable) == 0);
  assert(resumed.current == 1 && !resumed.ready);
}

static void persistence_failure_is_transactional(void) {
  const char* actions[] = {"confirm", "fail", "skip", "retry", "reset"};
  for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
    h2_mfg_model_t m = fresh();
    m.ready = 1;
    h2_mfg_model_t before = m;
    h2_loader_mfg_summary_t stored = durable;
    save_result = H2_PAL_ERR_IO;
    assert(h2_mfg_model_action(&m, actions[i], i == 4 ? "all" : "storage",
                               m.ticket) == H2_PAL_ERR_IO);
    assert(memcmp(&m, &before, sizeof(m)) == 0);
    assert(memcmp(&durable, &stored, sizeof(stored)) == 0);
  }
}

static void fail_skip_retry_and_gate(void) {
  h2_mfg_model_t m = fresh();
  assert(h2_mfg_model_action(&m, "fail", "storage", m.ticket) == 0);
  m.ready = 1;
  assert(h2_mfg_model_action(&m, "confirm", "storage", m.ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(h2_mfg_model_action(&m, "retry", "storage", m.ticket) == 0);
  assert(!m.ready && durable.step_status[0] == 0);
  assert(h2_mfg_model_action(&m, "skip", "storage", m.ticket) == 0);
  assert(m.current == 1 && !h2_mfg_model_complete(&m));
  m.ready = 1;
  assert(h2_mfg_model_action(&m, "confirm", "display", m.ticket) == 0);
  assert(m.current == 0); /* A skipped required step still blocks the gate. */
  assert(h2_mfg_model_action(&m, "retry", "storage", m.ticket) == 0);
  m.ready = 1;
  assert(h2_mfg_model_action(&m, "confirm", "storage", m.ticket) == 0);
  assert(m.current == 2);
  m.ready = 1;
  m.error = H2_PAL_ERR_UNSUPPORTED;
  assert(h2_mfg_model_action(&m, "confirm", "touch", m.ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  m.error = 0;
  assert(h2_mfg_model_action(&m, "confirm", "touch", m.ticket) == 0);
  assert(h2_mfg_model_complete(&m));
  assert(h2_mfg_model_action(&m, "retry", "display", m.ticket) == 0);
  assert(!h2_mfg_model_complete(&m) && m.current == 1);
  assert(h2_mfg_model_action(&m, "reset", "all", m.ticket) == 0);
  assert(m.current == 0 &&
         memcmp(durable.step_status, (uint8_t[H2_LOADER_MFG_STEP_MAX]){0},
                H2_LOADER_MFG_STEP_MAX) == 0);
}

static void duplex_audio_requires_manual_aec(void) {
  const h2_mfg_step_t audio_steps[] = {
      {"speaker", H2_MFG_SPEAKER, 0},
      {"mic", H2_MFG_MIC, 0},
      {"aec", H2_MFG_AEC, 0},
  };
  h2_mfg_profile_t audio_profile = {
      .board = "duplex", .revision = 2, .steps = audio_steps, .total = 2};
  h2_loader_mfg_summary_t summary = {.total = 2};
  h2_mfg_model_t m;
  assert(h2_mfg_model_init(&m, &audio_profile, &summary, 1, persist,
                           &durable) == H2_PAL_ERR_INVALID_ARG);
  audio_profile.total = summary.total = 3;
  summary.step_status[0] = summary.step_status[1] = H2_LOADER_MFG_STEP_PASSED;
  assert(h2_mfg_model_init(&m, &audio_profile, &summary, 1, persist,
                           &durable) == 0);
  assert(m.current == 2 && !h2_mfg_model_complete(&m));
  assert(h2_mfg_model_action(&m, "confirm", "aec", m.ticket) ==
         H2_PAL_ERR_INVALID_STATE);
  h2_mfg_step_t automatic_aec[] = {{"aec", H2_MFG_AEC, 1}};
  audio_profile.steps = automatic_aec;
  audio_profile.total = summary.total = 1;
  memset(summary.step_status, 0, sizeof(summary.step_status));
  assert(h2_mfg_model_init(&m, &audio_profile, &summary, 1, persist,
                           &durable) == H2_PAL_ERR_INVALID_ARG);
}

int main(void) {
  confirmation_and_resume();
  persistence_failure_is_transactional();
  fail_skip_retry_and_gate();
  duplex_audio_requires_manual_aec();
  puts(
      "MFG model: readiness, stale tickets, durable resume, failed saves, "
      "failure/retry/reset and strict gate passed");
  return 0;
}
