#ifndef H2_IOS_AUDIO_WAIT_H
#define H2_IOS_AUDIO_WAIT_H

#include <pthread.h>
#include <stdint.h>

typedef struct h2_ios_audio_wait_budget {
  uint64_t deadline_ns;
  int infinite;
} h2_ios_audio_wait_budget_t;

int h2_ios_audio_wait_begin(uint32_t timeout_ms,
                           h2_ios_audio_wait_budget_t *out_budget);
int h2_ios_audio_wait_changed(pthread_cond_t *changed, pthread_mutex_t *mutex,
                             const h2_ios_audio_wait_budget_t *budget);

#endif
