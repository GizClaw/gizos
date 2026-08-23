#include "h2_web_platform_internal.h"

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <stdlib.h>

typedef struct h2_web_timer_token {
  h2_pal_timer_t *timer;
  uint64_t generation;
} h2_web_timer_token_t;

struct h2_pal_timer {
  h2_web_platform_t *owner;
  h2_pal_timer_t *next;
  h2_pal_timer_config_t config;
  h2_web_timer_token_t *token;
  uint64_t generation;
  uint64_t deadline_ms;
  long browser_id;
  bool running;
  bool pending;
  bool in_callback;
  bool destroy_pending;
};

static uint64_t h2_web_timer_now(void) {
  return (uint64_t)emscripten_get_now();
}

static uint64_t h2_web_timer_deadline_after(uint64_t now_ms,
                                            uint32_t period_ms) {
  return now_ms > UINT64_MAX - period_ms ? UINT64_MAX : now_ms + period_ms;
}

static void h2_web_timer_due(void *user) {
  h2_web_timer_token_t *token = user;
  h2_pal_timer_t *timer = token == NULL ? NULL : token->timer;
  if (timer != NULL && timer->token == token && timer->running &&
      timer->generation == token->generation && !timer->owner->shutting_down) {
    timer->token = NULL;
    timer->browser_id = 0;
    timer->pending = true;
    h2_web_platform_request_pump(timer->owner, h2_web_timer_now());
  }
  free(token);
}

static h2_pal_result_t h2_web_timer_arm(h2_pal_timer_t *timer,
                                        uint64_t deadline_ms) {
  h2_web_timer_token_t *token = malloc(sizeof(*token));
  if (token == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  token->timer = timer;
  token->generation = timer->generation;
  const uint64_t now_ms = h2_web_timer_now();
  const double delay_ms = deadline_ms > now_ms
                              ? (double)(deadline_ms - now_ms)
                              : 0.0;
  timer->deadline_ms = deadline_ms;
  timer->token = token;
  timer->browser_id =
      emscripten_set_timeout(h2_web_timer_due, delay_ms, token);
  return H2_PAL_OK;
}

static void h2_web_timer_cancel(h2_pal_timer_t *timer) {
  ++timer->generation;
  if (timer->token != NULL) {
    emscripten_clear_timeout(timer->browser_id);
    free(timer->token);
    timer->token = NULL;
  }
  timer->browser_id = 0;
  timer->pending = false;
}

static void h2_web_timer_unlink(h2_pal_timer_t *timer) {
  h2_pal_timer_t **cursor = &timer->owner->timers;
  while (*cursor != NULL) {
    if (*cursor == timer) {
      *cursor = timer->next;
      return;
    }
    cursor = &(*cursor)->next;
  }
}

static h2_pal_result_t h2_web_timer_start_impl(h2_pal_timer_t *timer) {
  if (timer->running) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  timer->running = true;
  ++timer->generation;
  const h2_pal_result_t result = h2_web_timer_arm(
      timer, h2_web_timer_deadline_after(h2_web_timer_now(),
                                         timer->config.period_ms));
  if (result != H2_PAL_OK) {
    timer->running = false;
  }
  return result;
}

static h2_pal_result_t h2_web_timer_create(
    void *user, const h2_pal_timer_config_t *config,
    h2_pal_timer_t **out_timer) {
  h2_web_platform_t *platform = user;
  if (out_timer != NULL) {
    *out_timer = NULL;
  }
  if (platform == NULL || config == NULL || out_timer == NULL ||
      config->cb == NULL || config->period_ms == 0u ||
      (config->flags & ~(H2_PAL_TIMER_FLAG_REPEAT |
                         H2_PAL_TIMER_FLAG_AUTO_START)) != 0u ||
      platform->shutting_down) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_pal_timer_t *timer = calloc(1u, sizeof(*timer));
  if (timer == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  timer->owner = platform;
  timer->config = *config;
  timer->next = platform->timers;
  platform->timers = timer;
  if ((config->flags & H2_PAL_TIMER_FLAG_AUTO_START) != 0u) {
    const h2_pal_result_t result = h2_web_timer_start_impl(timer);
    if (result != H2_PAL_OK) {
      h2_web_timer_unlink(timer);
      free(timer);
      return result;
    }
  }
  *out_timer = timer;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_timer_destroy(void *user,
                                             h2_pal_timer_t *timer) {
  if (user == NULL || timer == NULL || timer->owner != user) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_web_timer_cancel(timer);
  timer->running = false;
  if (timer->in_callback) {
    timer->destroy_pending = true;
    return H2_PAL_OK;
  }
  h2_web_timer_unlink(timer);
  free(timer);
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_timer_start(void *user,
                                           h2_pal_timer_t *timer) {
  if (user == NULL || timer == NULL || timer->owner != user) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_web_timer_start_impl(timer);
}

static h2_pal_result_t h2_web_timer_stop(void *user,
                                          h2_pal_timer_t *timer) {
  if (user == NULL || timer == NULL || timer->owner != user) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_web_timer_cancel(timer);
  timer->running = false;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_web_timer_reset(void *user,
                                           h2_pal_timer_t *timer) {
  if (user == NULL || timer == NULL || timer->owner != user) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_web_timer_cancel(timer);
  timer->running = true;
  return h2_web_timer_arm(timer,
                          h2_web_timer_deadline_after(
                              h2_web_timer_now(), timer->config.period_ms));
}

static h2_pal_result_t h2_web_timer_set_period(void *user,
                                                h2_pal_timer_t *timer,
                                                uint32_t period_ms) {
  if (user == NULL || timer == NULL || timer->owner != user ||
      period_ms == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  timer->config.period_ms = period_ms;
  return timer->running ? h2_web_timer_reset(user, timer) : H2_PAL_OK;
}

static h2_pal_result_t h2_web_timer_is_running(void *user,
                                                h2_pal_timer_t *timer,
                                                int *out_running) {
  if (out_running != NULL) {
    *out_running = 0;
  }
  if (user == NULL || timer == NULL || timer->owner != user ||
      out_running == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_running = timer->running ? 1 : 0;
  return H2_PAL_OK;
}

void h2_web_platform_timer_dispatch(h2_web_platform_t *platform) {
  h2_pal_timer_t *timer = platform->timers;
  while (timer != NULL) {
    if (timer->pending && timer->running) {
      timer->pending = false;
      const bool repeat =
          (timer->config.flags & H2_PAL_TIMER_FLAG_REPEAT) != 0u;
      if (!repeat) {
        timer->running = false;
      }
      timer->in_callback = true;
      timer->config.cb(timer->config.cb_user, timer);
      timer->in_callback = false;
      if (timer->destroy_pending) {
        h2_web_timer_unlink(timer);
        free(timer);
      } else if (repeat && timer->running && timer->token == NULL) {
        uint64_t deadline = timer->deadline_ms;
        const uint64_t now_ms = h2_web_timer_now();
        if (deadline <= now_ms) {
          const uint64_t periods =
              (now_ms - deadline) / timer->config.period_ms + 1u;
          if (periods > (UINT64_MAX - deadline) / timer->config.period_ms) {
            deadline = UINT64_MAX;
          } else {
            deadline += periods * timer->config.period_ms;
          }
        }
        if (deadline <= now_ms ||
            h2_web_timer_arm(timer, deadline) != H2_PAL_OK) {
          timer->running = false;
        }
      }
      /* A callback may destroy any timer in the list, so restart instead of
       * retaining a possibly freed next pointer across the callback. */
      timer = platform->timers;
      continue;
    }
    timer = timer->next;
  }
}

void h2_web_platform_timer_init(h2_web_platform_t *platform) {
  static const h2_pal_timer_vtable_t vtable = {
      .create = h2_web_timer_create,
      .destroy = h2_web_timer_destroy,
      .start = h2_web_timer_start,
      .stop = h2_web_timer_stop,
      .reset = h2_web_timer_reset,
      .set_period_ms = h2_web_timer_set_period,
      .is_running = h2_web_timer_is_running,
  };
  platform->timer_api = (h2_pal_timer_api_t){
      .user = platform,
      .vtable = &vtable,
  };
}

void h2_web_platform_timer_deinit(h2_web_platform_t *platform) {
  while (platform->timers != NULL) {
    h2_pal_timer_t *timer = platform->timers;
    platform->timers = timer->next;
    h2_web_timer_cancel(timer);
    free(timer);
  }
}
