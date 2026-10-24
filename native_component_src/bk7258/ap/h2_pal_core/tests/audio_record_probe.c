#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_record.h"
#include "onboard_mic_record.h"

static void* allocations[8];
static size_t live;
static int fail_allocation, fail_close;

void* os_malloc(size_t bytes) {
  if (fail_allocation) {
    fail_allocation = 0;
    return NULL;
  }
  for (size_t i = 0; i < 8; ++i) {
    if (allocations[i]) continue;
    allocations[i] = malloc(bytes);
    assert(allocations[i]);
    ++live;
    return allocations[i];
  }
  abort();
}
void os_free(void* pointer) {
  if (!pointer) return;
  for (size_t i = 0; i < 8; ++i) {
    if (allocations[i] != pointer) continue;
    free(pointer);
    allocations[i] = NULL;
    --live;
    return;
  }
  abort(); /* Includes duplicate frees. */
}

/* Hardware teardown is stubbed; the close body itself is extracted from SDK. */
typedef struct {
  audio_record_sta_t state;
} onboard_mic_record_priv_t;
static audio_record_sta_t onboard_mic_state_get(onboard_mic_record_priv_t* p) {
  return p->state;
}
static void onboard_mic_state_set(onboard_mic_record_priv_t* p,
                                  audio_record_sta_t state) {
  p->state = state;
}
static int onboard_mic_stop(onboard_mic_record_priv_t* p) {
  (void)p;
  return BK_OK;
}
static int onboard_mic_close(onboard_mic_record_priv_t* p) {
  (void)p;
  return BK_OK;
}
#define ONBOARD_MIC_RECORD_CHECK_NULL(p) \
  do {                                   \
    if (!(p)) return BK_FAIL;            \
  } while (0)
#define AUD_MIC_DATA_DUMP_CLOSE() ((void)0)
#define LOGD(...) ((void)0)
#define LOGE(...) ((void)0)
#define LOGV(...) ((void)0)
#define PM_DEV_ID_AUDIO 0
#define PM_CPU_FRQ_DEFAULT 0
#define bk_pm_module_vote_cpu_freq(device, frequency) ((void)0)
#include "sdk_mic_close.inc"

static int backend_open(audio_record_t* record, audio_record_cfg_t* config) {
  (void)config;
  onboard_mic_record_priv_t* p = os_malloc(sizeof(*p));
  if (!p) return BK_FAIL;
  p->state = AUDIO_RECORD_STA_RUNNING;
  record->record_ctx = p;
  return BK_OK;
}
static int backend_close(audio_record_t* record) {
  if (fail_close) return BK_FAIL;
  return onboard_mic_record_close(record);
}
static audio_record_ops_t ops = {.open = backend_open, .close = backend_close};
audio_record_ops_t* get_onboard_mic_record_ops(void) { return &ops; }

int main(void) {
  audio_record_cfg_t config = DEFAULT_AUDIO_RECORD_CONFIG();
  assert(audio_record_destroy(NULL) == BK_OK);
  for (unsigned i = 0; i < 128; ++i) {
    audio_record_t* record =
        audio_record_create(AUDIO_RECORD_ONBOARD_MIC, &config);
    assert(record && audio_record_open(record) == BK_OK);
    if (i % 2u == 0u) assert(audio_record_close(record) == BK_OK);
    assert(audio_record_destroy(record) == BK_OK);
    if (live != 0u) {
      printf("retained-context live=%zu iteration=%u\n", live, i);
      for (size_t j = 0; j < 8; ++j)
        if (allocations[j]) os_free(allocations[j]);
      return 19;
    }
  }
  audio_record_t* record =
      audio_record_create(AUDIO_RECORD_ONBOARD_MIC, &config);
  assert(record && audio_record_destroy(record) == BK_OK && live == 0u);
  fail_allocation = 1;
  assert(audio_record_create(AUDIO_RECORD_ONBOARD_MIC, &config) == NULL);
  record = audio_record_create(AUDIO_RECORD_ONBOARD_MIC, &config);
  assert(record);
  fail_allocation = 1;
  assert(audio_record_open(record) == BK_FAIL);
  assert(audio_record_destroy(record) == BK_OK && live == 0u);
  record = audio_record_create(AUDIO_RECORD_ONBOARD_MIC, &config);
  assert(record && audio_record_open(record) == BK_OK);
  fail_close = 1;
  assert(audio_record_destroy(record) == BK_FAIL && live == 2u);
  fail_close = 0;
  assert(audio_record_destroy(record) == BK_OK && live == 0u);
  puts(
      "PASS 128 cycles, closed/unopened/NULL, allocation failure, retained failed close and retry");
  return 0;
}
