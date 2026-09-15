#include "h2_gizclaw_session.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_session_internal.h"
#include "h2_runtime.h"

#include <stdio.h>
#include <string.h>

struct h2_gizclaw_session {
  h2_gizclaw_session_config_t config;
  h2_pal_mutex_t *mutex;
  h2_pal_cond_t *progress;
  size_t waiters;
  uint64_t cancel_epoch;
  h2_gizclaw_session_state_t state;
  bool busy;
  bool workspace_rpc_active;
  bool restarting_input; /* Owns the route across cancellation dispatch. */
  bool closed;
  uint64_t operation_generation;
  uint64_t deadline;
  uint8_t *catalog_data;
  h2_gizclaw_workflow_page_t catalog;
  h2_gizclaw_conversation_t *conversation;
  bool conversation_running;
  /* WAITING ends once downstream audio passes wait_mark, or at the
   * deadline. */
  size_t wait_mark;
  uint64_t wait_deadline_ms;
  h2_gizclaw_conversation_callback_fn callback;
  h2_gizclaw_conversation_completion_fn completion;
  void *user;
};

/* Called with the Session mutex held; never inspect Conversation-owned state. */
static void record_audio_level(const h2_gizclaw_session_t *s, const char *stage,
                            h2_gizclaw_audio_log_t *log, h2_pal_result_t rc,
                            uint64_t generation, int detail,
                            h2_pal_log_level_t level) {
  char *message = h2_gizclaw_audio_log_append_internal(log, level);
  if (message == NULL)
    return;
  (void)snprintf(message, H2_PAL_LOG_MESSAGE_MAX,
                 "session=%p conversation=%p stage=%s rc=%d event_generation=%llu "
                 "detail=%d open=%d running=%d restarting=%d state=%d "
                 "closed=%d busy=%d rpc=%d workspace=%d gen=%llu",
                 (const void *)s, (void *)s->conversation, stage, (int)rc,
                 (unsigned long long)generation, detail,
                 s->state.conversation_input_open, s->conversation_running,
                 s->restarting_input, (int)s->state.conversation, s->closed,
                 s->busy, s->workspace_rpc_active, (int)s->state.workspace,
                 (unsigned long long)s->state.generation);
}

static void record_audio(const h2_gizclaw_session_t *s, const char *stage,
                      h2_gizclaw_audio_log_t *log, h2_pal_result_t rc,
                         uint64_t generation, int detail) {
  record_audio_level(s, stage, log, rc, generation, detail,
                  rc == H2_PAL_OK ? H2_PAL_LOG_DEBUG : H2_PAL_LOG_ERROR);
}

static h2_pal_result_t lock(h2_gizclaw_session_t *session) {
  return h2_pal_mutex_lock(session->config.sync, session->mutex);
}
static void unlock(h2_gizclaw_session_t *session) {
  (void)h2_pal_mutex_unlock(session->config.sync, session->mutex);
}
static void unlock_audio(h2_gizclaw_session_t *s, h2_gizclaw_audio_log_t *log) {
  h2_gizclaw_service_t *service = s->config.service;
  unlock(s);
  h2_gizclaw_service_flush_audio_log_internal(service, log);
}
static void changed(h2_gizclaw_session_t *session) {
  ++session->state.revision;
  if (session->config.runtime != NULL)
    h2_runtime_notify(session->config.runtime);
}
static bool text_valid(const char *text, size_t max) {
  return text != NULL && text[0] != '\0' && strlen(text) <= max;
}
static bool same(const char *a, const char *b) {
  return a != NULL && b != NULL && strcmp(a, b) == 0;
}
static h2_gizclaw_str_t str(const char *text) {
  return (h2_gizclaw_str_t){text, text != NULL ? strlen(text) : 0u};
}
static bool patch_valid(const h2_gizclaw_workspace_parameters_patch_t *p) {
  return p == NULL ||
         ((p->has_input || p->has_initiative ||
           p->has_agent_initiative_policy) &&
          (!p->has_input ||
           p->input == H2_GIZCLAW_WORKSPACE_INPUT_PUSH_TO_TALK ||
           p->input == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME) &&
          (!p->has_initiative ||
           p->initiative == H2_GIZCLAW_CONVERSATION_INITIATIVE_PEER ||
           p->initiative == H2_GIZCLAW_CONVERSATION_INITIATIVE_AGENT) &&
          (!p->has_agent_initiative_policy ||
           p->agent_initiative_policy ==
               H2_GIZCLAW_AGENT_INITIATIVE_ONCE_WHEN_EMPTY ||
           p->agent_initiative_policy ==
               H2_GIZCLAW_AGENT_INITIATIVE_ON_RELOAD));
}
static bool patch_same(const h2_gizclaw_workspace_parameters_patch_t *a,
                       const h2_gizclaw_workspace_parameters_patch_t *b) {
  return (!b->has_input || (a->has_input && a->input == b->input)) &&
         (!b->has_initiative ||
          (a->has_initiative && a->initiative == b->initiative)) &&
         (!b->has_agent_initiative_policy ||
          (a->has_agent_initiative_policy &&
           a->agent_initiative_policy == b->agent_initiative_policy));
}

h2_pal_result_t
h2_gizclaw_session_create(const h2_gizclaw_session_config_t *config,
                          h2_gizclaw_session_t **out_session) {
  if (out_session == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out_session = NULL;
  if (config == NULL || config->service == NULL || config->mem == NULL ||
      config->sync == NULL || config->time == NULL ||
      config->collections == NULL || config->collection_count == 0u ||
      config->max_workflows == 0u ||
      config->max_workflows > SIZE_MAX / sizeof(h2_gizclaw_workflow_t) ||
      config->catalog_bytes == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  for (size_t i = 0u; i < config->collection_count; ++i) {
    if (!text_valid(config->collections[i],
                    H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES))
      return H2_PAL_ERR_INVALID_ARG;
    for (size_t j = 0u; j < i; ++j)
      if (same(config->collections[i], config->collections[j]))
        return H2_PAL_ERR_INVALID_ARG;
  }
  h2_gizclaw_session_t *session =
      h2_pal_mem_alloc(config->mem, sizeof(*session));
  if (session == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(session, 0, sizeof(*session));
  session->config = *config;
  const h2_pal_mutex_config_t mutex_config = {.name = "gizclaw-session",
                                              .allocator = config->mem};
  h2_pal_result_t rc =
      h2_pal_mutex_create(config->sync, &mutex_config, &session->mutex);
  if (rc != H2_PAL_OK) {
    h2_pal_mem_free(config->mem, session);
    return rc;
  }
  const h2_pal_cond_config_t cond_config = {.name = "gizclaw-session",
                                            .allocator = config->mem};
  rc = h2_pal_cond_create(config->sync, &cond_config, &session->progress);
  if (rc != H2_PAL_OK) {
    (void)h2_pal_mutex_destroy(config->sync, session->mutex);
    h2_pal_mem_free(config->mem, session);
    return rc;
  }
  session->state.generation = 1u;
  rc = h2_gizclaw_service_attach_session_internal(config->service, session);
  if (rc != H2_PAL_OK) {
    (void)h2_pal_cond_destroy(config->sync, session->progress);
    (void)h2_pal_mutex_destroy(config->sync, session->mutex);
    h2_pal_mem_free(config->mem, session);
    return rc;
  }
  *out_session = session;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_session_destroy(h2_gizclaw_session_t **ptr) {
  if (ptr == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_session_t *session = *ptr;
  if (session == NULL)
    return H2_PAL_OK;
  h2_pal_result_t rc = lock(session);
  if (rc != H2_PAL_OK)
    return rc;
  bool busy = session->busy || session->workspace_rpc_active ||
              session->waiters != 0u || session->conversation != NULL;
  unlock(session);
  if (busy)
    return H2_PAL_ERR_BUSY;
  rc = h2_gizclaw_service_detach_session_internal(session->config.service);
  if (rc != H2_PAL_OK)
    return rc;
  if (session->progress != NULL) {
    rc = h2_pal_cond_destroy(session->config.sync, session->progress);
    if (rc != H2_PAL_OK)
      return rc;
    session->progress = NULL;
  }
  rc = h2_pal_mutex_destroy(session->config.sync, session->mutex);
  if (rc != H2_PAL_OK)
    return rc;
  h2_pal_mem_free(session->config.mem, session->catalog_data);
  h2_pal_mem_free(session->config.mem, session);
  *ptr = NULL;
  return H2_PAL_OK;
}

h2_pal_result_t h2_gizclaw_session_snapshot(h2_gizclaw_session_t *session,
                                            h2_gizclaw_session_state_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (session == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(session);
  if (rc != H2_PAL_OK)
    return rc;
  if (session->state.conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING) {
    /* Downstream sound takes over from WAITING; without any, give up
     * silently at the deadline. */
    uint64_t now = 0u;
    if (h2_gizclaw_conversation_downlink_writes_internal(
            session->config.service) != session->wait_mark ||
        (h2_pal_time_get_monotonic_ms(session->config.time, &now) ==
             H2_PAL_OK &&
         now >= session->wait_deadline_ms)) {
      session->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
      ++session->state.revision;
    }
  }
  *out = session->state;
  out->blocking_reason =
      session->closed ? H2_GIZCLAW_SESSION_BLOCK_CLOSED
      : out->registration != H2_GIZCLAW_SESSION_READY
          ? H2_GIZCLAW_SESSION_BLOCK_REGISTRATION
      : out->catalog != H2_GIZCLAW_SESSION_READY
          ? H2_GIZCLAW_SESSION_BLOCK_CATALOG
      : (out->workspace != H2_GIZCLAW_SESSION_READY || session->busy)
          ? H2_GIZCLAW_SESSION_BLOCK_WORKSPACE
      : session->conversation != NULL ? H2_GIZCLAW_SESSION_BLOCK_CONVERSATION
                                      : H2_GIZCLAW_SESSION_BLOCK_NONE;
  out->can_start = out->blocking_reason == H2_GIZCLAW_SESSION_BLOCK_NONE;
  unlock(session);
  return H2_PAL_OK;
}

static char *copy_string(const h2_pal_mem_api_t *mem, const char *text) {
  if (text == NULL)
    return NULL;
  size_t len = strlen(text) + 1u;
  char *copy = h2_pal_mem_alloc(mem, len);
  if (copy != NULL)
    memcpy(copy, text, len);
  return copy;
}

h2_pal_result_t
h2_gizclaw_session_catalog_copy(h2_gizclaw_session_t *session,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_workflow_page_t *out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  if (session == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_resp_arena_t arena;
  h2_pal_result_t rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  rc = lock(session);
  if (rc != H2_PAL_OK)
    return rc;
  if (session->closed || session->state.catalog != H2_GIZCLAW_SESSION_READY) {
    unlock(session);
    return H2_PAL_ERR_UNAVAILABLE;
  }
  h2_gizclaw_workflow_page_t page = {.count = session->catalog.count};
  const h2_pal_mem_api_t *mem = &arena.allocator;
  page.runtime_profile_name = copy_string(mem, session->state.profile_name);
  page.runtime_profile_revision =
      copy_string(mem, session->state.profile_revision);
  if (page.count != 0u) {
    page.items = h2_pal_mem_alloc(mem, page.count * sizeof(*page.items));
    if (page.items == NULL) {
      unlock(session);
      return h2_gizclaw_resp_arena_end(&arena, H2_PAL_ERR_NO_SPACE);
    }
  }
  for (size_t i = 0u; i < page.count; ++i) {
    const h2_gizclaw_workflow_t *src = &session->catalog.items[i];
    h2_gizclaw_workflow_t *dst = &page.items[i];
    *dst = (h2_gizclaw_workflow_t){
        .collection = copy_string(mem, src->collection),
        .name = copy_string(mem, src->name),
        .workspace_lang_pair = copy_string(mem, src->workspace_lang_pair),
        .i18n_count = src->i18n_count,
    };
    if (dst->i18n_count == 0u)
      continue;
    dst->i18n = h2_pal_mem_alloc(mem, dst->i18n_count * sizeof(*dst->i18n));
    if (dst->i18n == NULL) {
      rc = H2_PAL_ERR_NO_SPACE;
      break;
    }
    for (size_t j = 0u; j < dst->i18n_count; ++j) {
      dst->i18n[j] = (h2_gizclaw_workflow_i18n_t){
          .locale = copy_string(mem, src->i18n[j].locale),
          .display_name = copy_string(mem, src->i18n[j].display_name),
          .description = copy_string(mem, src->i18n[j].description),
      };
    }
  }
  unlock(session);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out = page;
  return rc;
}

static h2_pal_result_t begin(h2_gizclaw_session_t *s, uint32_t timeout,
                             bool wait, bool allow_conversation) {
  if (s == NULL || timeout == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  uint64_t now = 0u;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(s->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (UINT64_MAX - now < timeout)
    return H2_PAL_ERR_INVALID_ARG;
  rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  const uint64_t deadline = now + timeout;
  const uint64_t cancel_epoch = s->cancel_epoch;
  while (wait && s->busy && !s->closed && cancel_epoch == s->cancel_epoch) {
    ++s->waiters;
    rc = h2_pal_cond_wait(s->config.sync, s->progress, s->mutex,
                          (uint32_t)(deadline - now));
    --s->waiters;
    if (rc != H2_PAL_OK)
      break;
    rc = h2_pal_time_get_monotonic_ms(s->config.time, &now);
    if (rc != H2_PAL_OK || now >= deadline) {
      if (rc == H2_PAL_OK)
        rc = H2_PAL_ERR_TIMEOUT;
      break;
    }
  }
  if (rc != H2_PAL_OK) {
    unlock(s);
    return rc;
  }
  if (cancel_epoch != s->cancel_epoch) {
    unlock(s);
    return H2_PAL_ERR_CLOSED;
  }
  if (s->closed)
    rc = H2_PAL_ERR_CLOSED;
  else if (s->busy || s->workspace_rpc_active ||
           (!allow_conversation && s->conversation != NULL))
    rc = H2_PAL_ERR_BUSY;
  else {
    s->busy = true;
    s->operation_generation = s->state.generation;
    s->deadline = deadline;
  }
  unlock(s);
  return rc;
}
static h2_pal_result_t remaining(h2_gizclaw_session_t *s, uint32_t *timeout) {
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  bool closed = s->closed || s->operation_generation != s->state.generation;
  unlock(s);
  if (closed)
    return H2_PAL_ERR_CLOSED;
  uint64_t now = 0u;
  rc = h2_pal_time_get_monotonic_ms(s->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (now >= s->deadline)
    return H2_PAL_ERR_TIMEOUT;
  *timeout = (uint32_t)(s->deadline - now);
  return H2_PAL_OK;
}
static h2_pal_result_t finish(h2_gizclaw_session_t *s, h2_pal_result_t result,
                              h2_gizclaw_session_blocker_t stage) {
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->closed || s->operation_generation != s->state.generation)
    result = H2_PAL_ERR_CLOSED;
  s->busy = false;
  (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
  s->state.last_error = result;
  s->state.error_code[0] = '\0';
  s->state.retryable = false;
  if (stage == H2_GIZCLAW_SESSION_BLOCK_WORKSPACE) {
    if (s->state.registration != H2_GIZCLAW_SESSION_READY)
      stage = H2_GIZCLAW_SESSION_BLOCK_REGISTRATION;
    else if (s->state.catalog != H2_GIZCLAW_SESSION_READY)
      stage = H2_GIZCLAW_SESSION_BLOCK_CATALOG;
  }
  s->state.error_stage =
      result == H2_PAL_OK ? H2_GIZCLAW_SESSION_BLOCK_NONE : stage;
  changed(s);
  unlock(s);
  return result;
}

static h2_pal_result_t refresh(h2_gizclaw_session_t *s) {
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->closed || s->state.registration != H2_GIZCLAW_SESSION_READY) {
    unlock(s);
    return H2_PAL_ERR_UNAVAILABLE;
  }
  s->state.catalog = H2_GIZCLAW_SESSION_PREPARING;
  char expected[H2_GIZCLAW_REGISTRATION_NAME_CAPACITY];
  memcpy(expected, s->state.profile_name, sizeof(expected));
  changed(s);
  unlock(s);
  uint8_t *data = h2_pal_mem_alloc(s->config.mem, s->config.catalog_bytes);
  h2_gizclaw_resp_storage_t storage = {data, s->config.catalog_bytes, 0u};
  h2_gizclaw_resp_arena_t arena;
  h2_gizclaw_workflow_page_t catalog = {0};
  if (data == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  rc = h2_gizclaw_resp_arena_begin(&storage, &arena);
  if (rc != H2_PAL_OK)
    goto done;
  catalog.items = h2_pal_mem_alloc(
      &arena.allocator, s->config.max_workflows * sizeof(*catalog.items));
  rc = h2_gizclaw_resp_arena_end(&arena, H2_PAL_OK);
  if (rc != H2_PAL_OK)
    goto done;
  for (size_t c = 0u; c < s->config.collection_count && rc == H2_PAL_OK; ++c) {
    const char *cursor = NULL;
    /* Bound pages independently of item count, including empty-page cycles. */
    for (size_t p = 0u;; ++p) {
      if (p >= s->config.max_workflows + 1u) {
        rc = H2_PAL_ERR_FORMAT;
        break;
      }
      uint32_t timeout = 0u;
      rc = remaining(s, &timeout);
      if (rc != H2_PAL_OK)
        break;
      h2_gizclaw_workflow_page_t page = {0};
      rc = h2_gizclaw_rpc_workflow_list(
          s->config.service, str(s->config.collections[c]), str(cursor),
          H2_GIZCLAW_WORKFLOW_PAGE_MAX_ITEMS, timeout, &storage, &page);
      if (rc != H2_PAL_OK)
        break;
      if (!same(page.runtime_profile_name, expected) ||
          !text_valid(page.runtime_profile_revision,
                      sizeof(s->state.profile_revision) - 1u) ||
          (catalog.runtime_profile_revision != NULL &&
           !same(page.runtime_profile_revision,
                 catalog.runtime_profile_revision))) {
        rc = H2_PAL_ERR_INVALID_STATE;
        break;
      }
      catalog.runtime_profile_name = page.runtime_profile_name;
      catalog.runtime_profile_revision = page.runtime_profile_revision;
      if (page.count > s->config.max_workflows - catalog.count) {
        rc = H2_PAL_ERR_NO_SPACE;
        break;
      }
      for (size_t i = 0u; i < page.count; ++i) {
        if (!same(page.items[i].collection, s->config.collections[c]) ||
            !text_valid(page.items[i].name,
                        H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES)) {
          rc = H2_PAL_ERR_FORMAT;
          break;
        }
        for (size_t j = 0u; j < catalog.count; ++j)
          if (same(catalog.items[j].name, page.items[i].name))
            rc = H2_PAL_ERR_FORMAT;
        if (rc != H2_PAL_OK)
          break;
        catalog.items[catalog.count++] = page.items[i];
      }
      if (rc != H2_PAL_OK || !page.has_next)
        break;
      if (!text_valid(page.next_cursor, 4096u) ||
          same(cursor, page.next_cursor)) {
        rc = H2_PAL_ERR_FORMAT;
        break;
      }
      cursor = page.next_cursor;
    }
  }
done:
  if (rc == H2_PAL_OK) {
    uint32_t left = 0u;
    rc = remaining(s, &left);
  }
  {
    h2_pal_result_t lock_rc = lock(s);
    if (lock_rc != H2_PAL_OK) {
      h2_pal_mem_free(s->config.mem, data);
      return lock_rc;
    }
    if (s->closed || s->operation_generation != s->state.generation)
      rc = H2_PAL_ERR_CLOSED;
    if (rc == H2_PAL_OK) {
      if (!same(s->state.profile_revision, catalog.runtime_profile_revision)) {
        s->state.workspace = H2_GIZCLAW_SESSION_EMPTY;
        s->state.current_workspace[0] = '\0';
        memset(&s->state.parameters, 0, sizeof(s->state.parameters));
      }
      h2_pal_mem_free(s->config.mem, s->catalog_data);
      s->catalog_data = data;
      data = NULL;
      s->catalog = catalog;
      strcpy(s->state.profile_revision, catalog.runtime_profile_revision);
      s->state.workflow_count = catalog.count;
    }
    s->state.catalog = s->closed         ? H2_GIZCLAW_SESSION_CLOSED
                       : rc == H2_PAL_OK ? H2_GIZCLAW_SESSION_READY
                                         : H2_GIZCLAW_SESSION_FAILED;
    changed(s);
    unlock(s);
    h2_pal_mem_free(s->config.mem, data);
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_session_refresh(h2_gizclaw_session_t *s,
                                           uint32_t timeout) {
  h2_pal_result_t rc = begin(s, timeout, false, false);
  if (rc != H2_PAL_OK)
    return rc;
  return finish(s, refresh(s), H2_GIZCLAW_SESSION_BLOCK_CATALOG);
}

h2_pal_result_t h2_gizclaw_session_register(h2_gizclaw_session_t *s,
                                            const char *token,
                                            uint32_t timeout) {
  if (token == NULL || token[0] == '\0')
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = begin(s, timeout, false, false);
  if (rc != H2_PAL_OK)
    return rc;
  rc = lock(s);
  if (rc != H2_PAL_OK)
    return finish(s, rc, H2_GIZCLAW_SESSION_BLOCK_REGISTRATION);
  if (s->closed || s->operation_generation != s->state.generation) {
    unlock(s);
    return finish(s, H2_PAL_ERR_CLOSED, H2_GIZCLAW_SESSION_BLOCK_REGISTRATION);
  }
  s->state.registration = H2_GIZCLAW_SESSION_PREPARING;
  s->state.catalog = H2_GIZCLAW_SESSION_EMPTY;
  s->state.workspace = H2_GIZCLAW_SESSION_EMPTY;
  s->state.current_workspace[0] = '\0';
  s->state.target_workspace[0] = '\0';
  s->state.profile_name[0] = '\0';
  s->state.profile_revision[0] = '\0';
  memset(&s->state.parameters, 0, sizeof(s->state.parameters));
  ++s->state.generation;
  s->operation_generation = s->state.generation;
  changed(s);
  unlock(s);
  h2_gizclaw_registration_result_t result = {0};
  rc = remaining(s, &timeout);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_rpc_register(s->config.service, token, timeout, &result);
  if (rc == H2_PAL_OK && !text_valid(result.runtime_profile_name,
                                     sizeof(s->state.profile_name) - 1u))
    rc = H2_PAL_ERR_FORMAT;
  h2_pal_result_t lock_rc = lock(s);
  if (lock_rc != H2_PAL_OK)
    return finish(s, lock_rc, H2_GIZCLAW_SESSION_BLOCK_REGISTRATION);
  if (s->closed || s->operation_generation != s->state.generation)
    rc = H2_PAL_ERR_CLOSED;
  s->state.registration = s->closed         ? H2_GIZCLAW_SESSION_CLOSED
                          : rc == H2_PAL_OK ? H2_GIZCLAW_SESSION_READY
                                            : H2_GIZCLAW_SESSION_FAILED;
  if (rc == H2_PAL_OK)
    strcpy(s->state.profile_name, result.runtime_profile_name);
  changed(s);
  unlock(s);
  h2_gizclaw_session_blocker_t stage = H2_GIZCLAW_SESSION_BLOCK_REGISTRATION;
  if (rc == H2_PAL_OK) {
    stage = H2_GIZCLAW_SESSION_BLOCK_CATALOG;
    rc = refresh(s);
  }
  return finish(s, rc, stage);
}

/* WorkspaceGetResponse has no collection field in SDK 0.15.6. Validate the
 * Workflow/collection pair against the registered catalog before this get;
 * the public workspace.collection is populated only by scoped list calls. */
static bool workspace_matches(const h2_gizclaw_workspace_get_result_t *r,
                              const h2_gizclaw_session_selection_t *selection,
                              const char *profile, const char *revision) {
  return same(r->workspace.name, selection->workspace_name) &&
         r->workspace.available &&
         (selection->workflow_name == NULL ||
          same(r->workspace.workflow_name, selection->workflow_name)) &&
         same(r->runtime_profile_name, profile) &&
         same(r->runtime_profile_revision, revision);
}

/* Caller holds the Session mutex or owns the serialized preparation. */
static bool
catalog_contains_selection(const h2_gizclaw_session_t *s,
                           const h2_gizclaw_session_selection_t *selection) {
  if (selection->workflow_name == NULL)
    return true;
  for (size_t i = 0u; i < s->catalog.count; ++i)
    if (same(s->catalog.items[i].name, selection->workflow_name) &&
        same(s->catalog.items[i].collection, selection->collection))
      return true;
  return false;
}

static h2_pal_result_t
prepare_workspace(h2_gizclaw_session_t *s,
                  const h2_gizclaw_session_selection_t *selection) {
  uint32_t timeout = 0u;
  h2_pal_result_t rc = remaining(s, &timeout);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_session_state_t snapshot;
  rc = h2_gizclaw_session_snapshot(s, &snapshot);
  if (rc != H2_PAL_OK)
    return rc;
  if (!catalog_contains_selection(s, selection))
    return H2_PAL_ERR_NOT_FOUND;
  uint8_t *data = h2_pal_mem_alloc(s->config.mem, s->config.catalog_bytes);
  if (data == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  h2_gizclaw_resp_storage_t storage = {data, s->config.catalog_bytes, 0u};
  h2_gizclaw_workspace_get_result_t workspace = {0};
  rc = h2_gizclaw_rpc_workspace_get(s->config.service,
                                    str(selection->workspace_name), timeout,
                                    &storage, &workspace);
  if (rc == H2_PAL_ERR_NOT_FOUND && selection->workflow_name != NULL) {
    rc = remaining(s, &timeout);
    if (rc != H2_PAL_OK)
      goto done;
    storage.used = 0u;
    h2_gizclaw_workspace_t created = {0};
    h2_pal_result_t create_rc = h2_gizclaw_rpc_workspace_create(
        s->config.service, str(selection->collection),
        str(selection->workflow_name), str(selection->workspace_name), timeout,
        &storage, &created);
    /* Reconcile even an uncertain create with the same deterministic name. */
    rc = remaining(s, &timeout);
    if (rc != H2_PAL_OK)
      goto done;
    storage.used = 0u;
    rc = h2_gizclaw_rpc_workspace_get(s->config.service,
                                      str(selection->workspace_name), timeout,
                                      &storage, &workspace);
    if (rc != H2_PAL_OK && create_rc != H2_PAL_OK)
      rc = create_rc;
  }
  if (rc != H2_PAL_OK)
    goto done;
  if (!workspace_matches(&workspace, selection, snapshot.profile_name,
                         snapshot.profile_revision) ||
      !text_valid(workspace.workspace.workflow_name,
                  sizeof(snapshot.workflow_name) - 1u)) {
    rc = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  strcpy(snapshot.workflow_name, workspace.workspace.workflow_name);
  rc = remaining(s, &timeout);
  if (rc != H2_PAL_OK)
    goto done;
  storage.used = 0u;
  h2_gizclaw_workspace_activation_t activation = {0};
  rc = h2_gizclaw_rpc_workspace_reload_with_options(
      s->config.service, str(selection->workspace_name), selection->parameters,
      timeout, &storage, &activation);
  if (rc == H2_PAL_OK &&
      (activation.runtime_state != H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING ||
       !same(activation.active_workspace_name, selection->workspace_name) ||
       (activation.workflow_name != NULL &&
        !same(activation.workflow_name, snapshot.workflow_name))))
    rc = H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK)
    rc = remaining(s, &timeout);
  if (rc == H2_PAL_OK) {
    rc = lock(s);
    if (rc != H2_PAL_OK)
      goto done;
    if (s->closed || s->operation_generation != s->state.generation)
      rc = H2_PAL_ERR_CLOSED;
    else {
      strcpy(s->state.current_workspace, selection->workspace_name);
      strcpy(s->state.workflow_name, snapshot.workflow_name);
    }
    unlock(s);
  }
done:
  h2_pal_mem_free(s->config.mem, data);
  return rc;
}

static bool selection_valid(const h2_gizclaw_session_selection_t *selection) {
  return selection != NULL &&
         text_valid(selection->workspace_name,
                    H2_GIZCLAW_WORKSPACE_NAME_MAX_BYTES) &&
         ((selection->collection == NULL && selection->workflow_name == NULL) ||
          (text_valid(selection->collection,
                      H2_GIZCLAW_WORKFLOW_COLLECTION_MAX_BYTES) &&
           text_valid(selection->workflow_name,
                      H2_GIZCLAW_WORKFLOW_NAME_MAX_BYTES))) &&
         patch_valid(selection->parameters);
}

static h2_pal_result_t
select_workspace(h2_gizclaw_session_t *s,
                 const h2_gizclaw_session_selection_t *selection) {
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->closed || s->operation_generation != s->state.generation) {
    unlock(s);
    return H2_PAL_ERR_CLOSED;
  }
  bool ready = s->state.catalog == H2_GIZCLAW_SESSION_READY &&
               catalog_contains_selection(s, selection) &&
               s->state.workspace == H2_GIZCLAW_SESSION_READY &&
               same(s->state.current_workspace, selection->workspace_name) &&
               (selection->workflow_name == NULL ||
                same(s->state.workflow_name, selection->workflow_name)) &&
               (selection->parameters == NULL ||
                patch_same(&s->state.parameters, selection->parameters));
  strcpy(s->state.target_workspace, selection->workspace_name);
  if (ready) {
    unlock(s);
    return H2_PAL_OK;
  }
  bool catalog_ready = s->state.catalog == H2_GIZCLAW_SESSION_READY;
  s->state.workspace = H2_GIZCLAW_SESSION_PREPARING;
  changed(s);
  unlock(s);
  if (!catalog_ready)
    rc = refresh(s);
  if (rc == H2_PAL_OK)
    rc = prepare_workspace(s, selection);
  if (rc == H2_PAL_ERR_INVALID_STATE) {
    rc = refresh(s);
    if (rc == H2_PAL_OK)
      rc = prepare_workspace(s, selection);
  }
  h2_pal_result_t lock_rc = lock(s);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  if (s->closed || s->operation_generation != s->state.generation)
    rc = H2_PAL_ERR_CLOSED;
  s->state.workspace = s->closed         ? H2_GIZCLAW_SESSION_CLOSED
                       : rc == H2_PAL_OK ? H2_GIZCLAW_SESSION_READY
                                         : H2_GIZCLAW_SESSION_FAILED;
  /* After an uncertain reload the old name is retained for display only;
   * FAILED explicitly prevents treating it as confirmed ready. */
  changed(s);
  unlock(s);
  return rc;
}

h2_pal_result_t
h2_gizclaw_session_select(h2_gizclaw_session_t *s,
                          const h2_gizclaw_session_selection_t *selection,
                          uint32_t timeout) {
  if (!selection_valid(selection))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = begin(s, timeout, true, true);
  if (rc != H2_PAL_OK)
    return rc;
  return finish(s, select_workspace(s, selection),
                H2_GIZCLAW_SESSION_BLOCK_WORKSPACE);
}

h2_pal_result_t h2_gizclaw_session_close(h2_gizclaw_session_t *s) {
  if (s == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (!s->closed) {
    s->closed = true;
    (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
    ++s->state.generation;
    s->state.registration = H2_GIZCLAW_SESSION_CLOSED;
    s->state.catalog = H2_GIZCLAW_SESSION_CLOSED;
    s->state.workspace = H2_GIZCLAW_SESSION_CLOSED;
    s->state.current_workspace[0] = '\0';
    changed(s);
  }
  unlock(s);
  return H2_PAL_OK;
}

static h2_pal_result_t
conversation_event(void *user, h2_gizclaw_conversation_t *conversation,
                   const h2_gizclaw_conversation_event_t *event) {
  h2_gizclaw_audio_log_t logs = {0};
  h2_gizclaw_session_t *s = user;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (conversation != s->conversation || s->restarting_input ||
      s->state.workspace != H2_GIZCLAW_SESSION_READY ||
      s->state.conversation == H2_GIZCLAW_SESSION_CONVERSATION_IDLE) {
    if (event->kind != H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA)
      record_audio(s, "event_drop", &logs, H2_PAL_OK, event->generation, (int)event->kind);
    unlock_audio(s, &logs);
    return H2_PAL_OK;
  }
  if (event->kind != H2_GIZCLAW_CONVERSATION_EVENT_TEXT_DELTA)
    record_audio(s, "event_accept", &logs, H2_PAL_OK, event->generation, (int)event->kind);
  /* Text carries no state. ERROR is the server refusing this input. */
  if (event->kind == H2_GIZCLAW_CONVERSATION_EVENT_ERROR) {
    snprintf(s->state.error_code, sizeof(s->state.error_code), "%s",
             event->error_code != NULL ? event->error_code : "");
    s->state.retryable = event->retryable;
    s->state.last_error = H2_PAL_ERR_IO;
    s->state.error_stage = H2_GIZCLAW_SESSION_BLOCK_CONVERSATION;
    s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
    changed(s);
  }
  h2_gizclaw_conversation_callback_fn callback = s->callback;
  void *callback_user = s->user;
  unlock_audio(s, &logs);
  return callback != NULL ? callback(callback_user, conversation, event)
                          : H2_PAL_OK;
}
static void conversation_complete(void *user,
                                  h2_gizclaw_conversation_t *conversation,
                                  const h2_gizclaw_operation_result_t *result) {
  h2_gizclaw_audio_log_t logs = {0};
  h2_gizclaw_operation_result_t effective_result = *result;
  result = &effective_result;
  h2_gizclaw_session_t *s = user;
  if (lock(s) != H2_PAL_OK)
    return;
  if (conversation != s->conversation) {
    unlock_audio(s, &logs);
    return;
  }
  record_audio_level(s, "complete", &logs, result->result, 0u, (int)result->terminal_kind,
                  result->result == H2_PAL_OK ||
                          result->terminal_kind == H2_GIZCLAW_OPERATION_CANCELED
                      ? H2_PAL_LOG_DEBUG : H2_PAL_LOG_ERROR);
  s->conversation_running = false;
  (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
  if (s->restarting_input) {
    /* audio_start is replacing this generation on the same logical route. */
    unlock_audio(s, &logs);
    return;
  }
  if (!s->closed && !s->workspace_rpc_active &&
      s->state.workspace == H2_GIZCLAW_SESSION_READY &&
      s->state.conversation == H2_GIZCLAW_SESSION_CONVERSATION_CALLING &&
      result->terminal_kind != H2_GIZCLAW_OPERATION_CANCELED &&
      result->result == H2_PAL_OK) {
    h2_pal_result_t rc = h2_gizclaw_service_audio_control_internal(
        s->config.service, true, &logs, NULL);
    record_audio(s, "auto_audio_start", &logs, rc, 0u, 0);
    if (rc == H2_PAL_OK) {
      s->conversation_running = true;
      changed(s);
      unlock_audio(s, &logs);
      return;
    }
    effective_result.result = rc;
  }
  s->state.conversation_input_open = false;
  /* A push-to-talk input is complete once it is sent; the wait for sound
   * started at release goes on. Any failure ends it. */
  if (s->state.conversation != H2_GIZCLAW_SESSION_CONVERSATION_WAITING ||
      result->result != H2_PAL_OK)
    s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
  s->state.last_error = result->result;
  memcpy(s->state.error_code, result->error_code, sizeof(s->state.error_code));
  s->state.error_code[sizeof(s->state.error_code) - 1u] = '\0';
  s->state.retryable = result->retryable;
  s->state.error_stage = result->result == H2_PAL_OK
                             ? H2_GIZCLAW_SESSION_BLOCK_NONE
                             : H2_GIZCLAW_SESSION_BLOCK_CONVERSATION;
  changed(s);
  h2_gizclaw_conversation_completion_fn completion = s->completion;
  void *callback_user = s->user;
  unlock_audio(s, &logs);
  if (completion != NULL)
    completion(callback_user, conversation, result);
}

h2_pal_result_t h2_gizclaw_session_conversation_create(
    h2_gizclaw_session_t *s, const h2_gizclaw_session_selection_t *selection,
    uint32_t timeout, h2_gizclaw_conversation_callback_fn callback,
    h2_gizclaw_conversation_completion_fn completion, void *user,
    h2_gizclaw_conversation_t **out) {
  if (out == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  if (!selection_valid(selection))
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = begin(s, timeout, true, false);
  if (rc != H2_PAL_OK)
    return rc;
  rc = lock(s);
  if (rc != H2_PAL_OK)
    return finish(s, rc, H2_GIZCLAW_SESSION_BLOCK_CONVERSATION);
  s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
  changed(s);
  unlock(s);
  rc = select_workspace(s, selection);
  if (rc != H2_PAL_OK) {
    if (lock(s) == H2_PAL_OK) {
      s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
      changed(s);
      unlock(s);
    }
    return finish(s, rc, H2_GIZCLAW_SESSION_BLOCK_WORKSPACE);
  }
  rc = lock(s);
  if (rc != H2_PAL_OK)
    return finish(s, rc, H2_GIZCLAW_SESSION_BLOCK_CONVERSATION);
  if (s->closed || s->operation_generation != s->state.generation) {
    unlock(s);
    return finish(s, H2_PAL_ERR_CLOSED, H2_GIZCLAW_SESSION_BLOCK_CONVERSATION);
  }
  s->callback = callback;
  s->completion = completion;
  s->user = user;
  rc = h2_gizclaw_conversation_create(
      s->config.service, str(selection->workspace_name), conversation_event,
      conversation_complete, s, &s->conversation);
  s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
  if (rc == H2_PAL_OK)
    *out = s->conversation;
  s->busy = false;
  (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
  s->state.last_error = rc;
  s->state.error_code[0] = '\0';
  s->state.retryable = false;
  s->state.error_stage = rc == H2_PAL_OK
                             ? H2_GIZCLAW_SESSION_BLOCK_NONE
                             : H2_GIZCLAW_SESSION_BLOCK_CONVERSATION;
  changed(s);
  unlock(s);
  return rc;
}

void h2_gizclaw_session_conversation_release(
    h2_gizclaw_session_t *s, h2_gizclaw_conversation_t *conversation) {
  if (s == NULL || conversation == NULL || lock(s) != H2_PAL_OK)
    return;
  if (s->conversation == conversation && !s->conversation_running) {
    h2_gizclaw_conversation_release(conversation);
    s->conversation = NULL;
    (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
    s->state.conversation_input_open = false;
    /* A sent input's wait for sound outlives its route. */
    if (s->state.conversation != H2_GIZCLAW_SESSION_CONVERSATION_WAITING)
      s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
    changed(s);
  }
  unlock(s);
}

h2_pal_result_t h2_gizclaw_session_cancel_pending(h2_gizclaw_session_t *s) {
  if (s == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->busy || s->waiters != 0u) {
    ++s->cancel_epoch;
    (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
    ++s->state.generation;
    s->state.workspace = H2_GIZCLAW_SESSION_FAILED;
    changed(s);
  }
  unlock(s);
  return H2_PAL_OK;
}

/* Stop locally and wait only for cancellation dispatch, never for an agent
 * reply. The caller is a control task; service_poll runs independently. */
static h2_pal_result_t stop_conversation_locked(h2_gizclaw_session_t *s,
                                                uint32_t timeout,
                                                h2_gizclaw_audio_log_t *logs,
                                                h2_gizclaw_cancel_source_t source) {
  if (!s->conversation_running) {
    if (source != H2_GIZCLAW_CANCEL_RESTART)
      h2_gizclaw_conversation_downlink_flush_internal(s->config.service);
    return H2_PAL_OK;
  }
  if (s->state.conversation_input_open) {
    (void)h2_gizclaw_service_audio_control_internal(
        s->config.service, false, logs, NULL);
  }
  h2_pal_result_t rc =
      h2_gizclaw_conversation_cancel_internal(s->conversation, logs, source);
  if (rc != H2_PAL_OK)
    return rc;
  s->state.conversation_input_open = false;
  uint64_t now = 0u;
  rc = h2_pal_time_get_monotonic_ms(s->config.time, &now);
  if (rc != H2_PAL_OK)
    return rc;
  if (UINT64_MAX - now < timeout)
    return H2_PAL_ERR_INVALID_ARG;
  const uint64_t deadline = now + timeout;
  while (s->conversation_running && !s->closed) {
    ++s->waiters;
    rc = h2_pal_cond_wait(s->config.sync, s->progress, s->mutex,
                          (uint32_t)(deadline - now));
    --s->waiters;
    if (rc != H2_PAL_OK)
      return rc;
    rc = h2_pal_time_get_monotonic_ms(s->config.time, &now);
    if (rc != H2_PAL_OK)
      return rc;
    if (s->conversation_running && now >= deadline)
      return H2_PAL_ERR_TIMEOUT;
  }
  return s->closed ? H2_PAL_ERR_CLOSED : H2_PAL_OK;
}

/* Called with the Session mutex held after admission checks. Owns the
 * serialized workspace RPC slot and stops the active conversation first. */
static h2_pal_result_t
enter_workspace_rpc_locked(h2_gizclaw_session_t *s, uint32_t timeout,
                           h2_gizclaw_audio_log_t *logs) {
  s->workspace_rpc_active = true;
  s->state.workspace = H2_GIZCLAW_SESSION_PREPARING;
  s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
  changed(s);
  h2_pal_result_t rc =
      stop_conversation_locked(s, timeout, logs, H2_GIZCLAW_CANCEL_WORKSPACE);
  if (rc != H2_PAL_OK) {
    s->workspace_rpc_active = false;
    s->state.workspace = H2_GIZCLAW_SESSION_FAILED;
    s->state.last_error = rc;
    s->state.error_stage = H2_GIZCLAW_SESSION_BLOCK_WORKSPACE;
    changed(s);
  }
  return rc;
}

h2_pal_result_t h2_gizclaw_session_workspace_begin_internal(
    h2_gizclaw_session_t *s, h2_gizclaw_str_t name, uint32_t timeout) {
  h2_gizclaw_audio_log_t logs = {0};
  if (s == NULL)
    return H2_PAL_OK;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->workspace_rpc_active || s->restarting_input) {
    unlock_audio(s, &logs);
    return H2_PAL_ERR_BUSY;
  }
  if (s->closed) {
    unlock_audio(s, &logs);
    return H2_PAL_ERR_CLOSED;
  }
  const bool pending_selection =
      s->state.workspace == H2_GIZCLAW_SESSION_PREPARING;
  if (name.len != 0u) {
    memcpy(s->state.target_workspace, name.data, name.len);
    s->state.target_workspace[name.len] = '\0';
  } else if (!pending_selection || s->state.target_workspace[0] == '\0') {
    strcpy(s->state.target_workspace, s->state.current_workspace);
  }
  rc = enter_workspace_rpc_locked(s, timeout, &logs);
  unlock_audio(s, &logs);
  return rc;
}

h2_pal_result_t h2_gizclaw_session_workspace_delete_begin_internal(
    h2_gizclaw_session_t *s, h2_gizclaw_str_t name, uint32_t timeout,
    bool *out_participating) {
  h2_gizclaw_audio_log_t logs = {0};
  *out_participating = false;
  if (s == NULL)
    return H2_PAL_OK;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  const size_t current_len = strlen(s->state.current_workspace);
  const bool current = name.len != 0u && name.len == current_len &&
                       memcmp(name.data, s->state.current_workspace,
                              current_len) == 0;
  if (s->closed)
    rc = H2_PAL_ERR_CLOSED;
  else if (!current)
    rc = H2_PAL_OK; /* Other Workspaces do not affect this Session. */
  else if (s->workspace_rpc_active || s->restarting_input)
    rc = H2_PAL_ERR_BUSY;
  else {
    rc = enter_workspace_rpc_locked(s, timeout, &logs);
    *out_participating = rc == H2_PAL_OK;
  }
  unlock_audio(s, &logs);
  return rc;
}

h2_pal_result_t h2_gizclaw_session_workspace_delete_finish_internal(
    h2_gizclaw_session_t *s, h2_pal_result_t result) {
  if (s == NULL)
    return result;
  h2_pal_result_t lock_rc = lock(s);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  s->workspace_rpc_active = false;
  if (!s->closed &&
      (!s->busy || s->operation_generation == s->state.generation)) {
    if (result == H2_PAL_OK) {
      /* The deleted Workspace's identity and confirmed parameters are gone;
       * the next select must get/create/reload it from scratch. */
      if (same(s->state.target_workspace, s->state.current_workspace))
        s->state.target_workspace[0] = '\0';
      s->state.workspace = H2_GIZCLAW_SESSION_EMPTY;
      s->state.current_workspace[0] = '\0';
      s->state.workflow_name[0] = '\0';
      memset(&s->state.parameters, 0, sizeof(s->state.parameters));
    } else {
      /* Deletion may have happened; never keep an uncertain name as READY. */
      s->state.workspace = H2_GIZCLAW_SESSION_FAILED;
    }
    s->state.last_error = result;
    s->state.error_stage = result == H2_PAL_OK
                               ? H2_GIZCLAW_SESSION_BLOCK_NONE
                               : H2_GIZCLAW_SESSION_BLOCK_WORKSPACE;
    changed(s);
  }
  unlock(s);
  return result;
}

h2_pal_result_t h2_gizclaw_session_workspace_finish_internal(
    h2_gizclaw_session_t *s, h2_pal_result_t result,
    const h2_gizclaw_workspace_activation_t *activation,
    const h2_gizclaw_workspace_parameters_patch_t *p) {
  if (s == NULL)
    return result;
  h2_pal_result_t lock_rc = lock(s);
  if (lock_rc != H2_PAL_OK)
    return lock_rc;
  s->workspace_rpc_active = false;
  if (!s->closed &&
      (!s->busy || s->operation_generation == s->state.generation)) {
    if (result == H2_PAL_OK && activation != NULL &&
        activation->runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING &&
        (activation->active_workspace_name == NULL ||
         (s->state.target_workspace[0] != '\0' &&
          !same(activation->active_workspace_name, s->state.target_workspace))))
      result = H2_PAL_ERR_FORMAT;
    if (result == H2_PAL_OK && activation != NULL &&
        activation->runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING &&
        activation->active_workspace_name != NULL && s->conversation != NULL &&
        (s->state.target_workspace[0] == '\0' ||
         same(activation->active_workspace_name, s->state.target_workspace))) {
      result = h2_gizclaw_conversation_retarget_internal(
          s->conversation, activation->active_workspace_name);
    }
    const bool applied =
        result == H2_PAL_OK &&
        (activation == NULL ||
         (activation->runtime_state == H2_GIZCLAW_WORKSPACE_RUNTIME_RUNNING &&
          activation->active_workspace_name != NULL &&
          (s->state.target_workspace[0] == '\0' ||
           same(activation->active_workspace_name,
                s->state.target_workspace))));
    s->state.workspace = result != H2_PAL_OK ? H2_GIZCLAW_SESSION_FAILED
                         : applied           ? H2_GIZCLAW_SESSION_READY
                                             : H2_GIZCLAW_SESSION_PREPARING;
    if (applied) {
      if (activation != NULL && activation->active_workspace_name != NULL)
        snprintf(s->state.current_workspace, sizeof(s->state.current_workspace),
                 "%s", activation->active_workspace_name);
      if (p != NULL) {
        if (p->has_input) {
          s->state.parameters.has_input = true;
          s->state.parameters.input = p->input;
        }
        if (p->has_initiative) {
          s->state.parameters.has_initiative = true;
          s->state.parameters.initiative = p->initiative;
        }
        if (p->has_agent_initiative_policy) {
          s->state.parameters.has_agent_initiative_policy = true;
          s->state.parameters.agent_initiative_policy =
              p->agent_initiative_policy;
        }
      }
    }
    s->state.last_error = result;
    s->state.error_stage = result == H2_PAL_OK
                               ? H2_GIZCLAW_SESSION_BLOCK_NONE
                               : H2_GIZCLAW_SESSION_BLOCK_WORKSPACE;
    changed(s);
  }
  unlock(s);
  return result;
}

static h2_pal_result_t audio_input(h2_gizclaw_session_t *s, bool start) {
  h2_gizclaw_audio_log_t logs = {0};
  if (s == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK) {
    char *message = h2_gizclaw_audio_log_append_internal(&logs, H2_PAL_LOG_ERROR);
    (void)snprintf(message, H2_PAL_LOG_MESSAGE_MAX,
                   "session=%p stage=%s_lock rc=%d", (void *)s,
                   start ? "audio_start" : "audio_end", (int)rc);
    h2_gizclaw_service_flush_audio_log_internal(s->config.service, &logs);
    return rc;
  }
  if (!start && s->conversation == NULL) {
    record_audio(s, "audio_end_no_conversation", &logs, H2_PAL_OK, 0u, 0);
    unlock_audio(s, &logs);
    return H2_PAL_OK;
  }
  if (s->closed || s->busy || s->workspace_rpc_active ||
      s->state.workspace != H2_GIZCLAW_SESSION_READY ||
      s->conversation == NULL) {
    record_audio(s, start ? "audio_start_rejected" : "audio_end_rejected", &logs,
              H2_PAL_ERR_INVALID_STATE, 0u, 0);
    unlock_audio(s, &logs);
    return H2_PAL_ERR_INVALID_STATE;
  }
  if ((!start && !s->state.conversation_input_open) ||
      (start && s->state.conversation_input_open)) {
    record_audio(s, start ? "audio_start_already_open" : "audio_end_not_open",
                 &logs, H2_PAL_OK, 0u, 0);
    unlock_audio(s, &logs);
    return H2_PAL_OK;
  }
  if (start && s->conversation_running) {
    s->busy = true;
    s->restarting_input = true;
    record_audio_level(s, "interrupt_begin", &logs, H2_PAL_OK, 0u, 0, H2_PAL_LOG_INFO);
    rc = stop_conversation_locked(s, 30000u, &logs, H2_GIZCLAW_CANCEL_RESTART);
    record_audio(s, "interrupt_drained", &logs, rc, 0u, 0);
    s->restarting_input = false;
    s->busy = false;
    (void)h2_pal_cond_broadcast(s->config.sync, s->progress);
    if (rc != H2_PAL_OK) {
      s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
      s->state.last_error = rc;
      changed(s);
      unlock_audio(s, &logs);
      return rc;
    }
  }
  bool empty_input = false;
  rc = h2_gizclaw_service_audio_control_internal(
      s->config.service, start, &logs, &empty_input);
  record_audio(s, start ? "audio_start" : "audio_end", &logs, rc, 0u, 0);
  if (rc == H2_PAL_OK) {
    s->state.conversation =
        s->state.parameters.input == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME
            ? (start ? H2_GIZCLAW_SESSION_CONVERSATION_CALLING
                     : H2_GIZCLAW_SESSION_CONVERSATION_IDLE)
            : (start ? H2_GIZCLAW_SESSION_CONVERSATION_RECORDING
                     : empty_input ? H2_GIZCLAW_SESSION_CONVERSATION_IDLE
                                   : H2_GIZCLAW_SESSION_CONVERSATION_WAITING);
    if (s->state.conversation == H2_GIZCLAW_SESSION_CONVERSATION_WAITING) {
      /* Released: wait for the server's sound, silently giving up after a
       * while. The release already cleared what was buffered before. */
      uint64_t now = 0u;
      (void)h2_pal_time_get_monotonic_ms(s->config.time, &now);
      s->wait_mark =
          h2_gizclaw_conversation_downlink_writes_internal(s->config.service);
      s->wait_deadline_ms = now + H2_GIZCLAW_SESSION_WAIT_MS;
    }
    s->state.conversation_input_open = start;
    if (!start &&
        s->state.parameters.input == H2_GIZCLAW_WORKSPACE_INPUT_REALTIME) {
      rc = h2_gizclaw_conversation_cancel_internal(
          s->conversation, &logs, H2_GIZCLAW_CANCEL_REALTIME_END);
      record_audio(s, "audio_end_cancel", &logs, rc, 0u, 0);
    }
    if (start) {
      s->conversation_running = true;
      s->state.last_error = H2_PAL_OK;
      s->state.error_stage = H2_GIZCLAW_SESSION_BLOCK_NONE;
      s->state.error_code[0] = '\0';
      s->state.retryable = false;
    }
    changed(s);
  } else if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_BUSY) {
    s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_IDLE;
    s->state.last_error = rc;
    s->state.error_stage = H2_GIZCLAW_SESSION_BLOCK_CONVERSATION;
    changed(s);
  }
  unlock_audio(s, &logs);
  return rc;
}
h2_pal_result_t h2_gizclaw_session_audio_start(h2_gizclaw_session_t *s) {
  return audio_input(s, true);
}
h2_pal_result_t h2_gizclaw_session_audio_end(h2_gizclaw_session_t *s) {
  return audio_input(s, false);
}

h2_pal_result_t h2_gizclaw_session_send_text(h2_gizclaw_session_t *s,
                                             h2_gizclaw_str_t text) {
  h2_gizclaw_audio_log_t logs = {0};
  if (s == NULL || text.data == NULL || text.len == 0u ||
      text.len > H2_GIZCLAW_CONVERSATION_TEXT_MAX_BYTES)
    return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = lock(s);
  if (rc != H2_PAL_OK)
    return rc;
  if (s->closed || s->busy || s->workspace_rpc_active ||
      s->state.workspace != H2_GIZCLAW_SESSION_READY ||
      s->conversation == NULL) {
    record_audio(s, "send_text_rejected", &logs, H2_PAL_ERR_INVALID_STATE, 0u, 0);
    unlock_audio(s, &logs);
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* Text never interrupts: an open input or a pending one must finish. */
  if (s->state.conversation_input_open || s->conversation_running) {
    record_audio(s, "send_text_busy", &logs, H2_PAL_ERR_BUSY, 0u, 0);
    unlock_audio(s, &logs);
    return H2_PAL_ERR_BUSY;
  }
  rc = h2_gizclaw_conversation_send_text_internal(s->conversation, text, &logs);
  record_audio(s, "send_text", &logs, rc, 0u, (int)text.len);
  if (rc == H2_PAL_OK) {
    /* Sent input awaits the server's sound, as after a push-to-talk release. */
    uint64_t now = 0u;
    (void)h2_pal_time_get_monotonic_ms(s->config.time, &now);
    s->wait_mark =
        h2_gizclaw_conversation_downlink_writes_internal(s->config.service);
    s->wait_deadline_ms = now + H2_GIZCLAW_SESSION_WAIT_MS;
    s->state.conversation = H2_GIZCLAW_SESSION_CONVERSATION_WAITING;
    s->conversation_running = true;
    s->state.last_error = H2_PAL_OK;
    s->state.error_stage = H2_GIZCLAW_SESSION_BLOCK_NONE;
    s->state.error_code[0] = '\0';
    s->state.retryable = false;
    changed(s);
  } else if (rc != H2_PAL_ERR_WOULD_BLOCK && rc != H2_PAL_ERR_BUSY &&
             rc != H2_PAL_ERR_INVALID_ARG) {
    s->state.last_error = rc;
    s->state.error_stage = H2_GIZCLAW_SESSION_BLOCK_CONVERSATION;
    changed(s);
  }
  unlock_audio(s, &logs);
  return rc;
}
