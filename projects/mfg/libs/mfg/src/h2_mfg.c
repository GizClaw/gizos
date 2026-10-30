#include "h2_mfg_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int save(void* user, const h2_loader_mfg_summary_t* summary) {
  h2_mfg_t* m = user;
  int rc = h2_loader_mfg_write(m->rt->pref, summary);
  /* All saves run with operation_mutex held. Refresh the Loader's in-memory
   * gate only after commit, including retry/reset which revoke acceptance. */
  if (rc == 0 && m->loader != NULL) m->loader->status.mfg = *summary;
  return rc;
}

static void drain_events(h2_mfg_t* m) {
  union {
    uint64_t alignment;
    uint8_t bytes[H2_RUNTIME_EVENT_PAYLOAD_MAX];
  } payload;
  h2_runtime_event_t event = {.payload = payload.bytes,
                              .payload_capacity = sizeof(payload.bytes)};
  /* Hardware is polled directly; consume Runtime's independent bridge of
   * network and BLE events so repeated radio operations cannot fill it. */
  for (unsigned i = 0; i < 64; ++i)
    if (h2_runtime_poll_event(m->rt, &event) != 0) break;
}

static void status(h2_mfg_t* m, char* out, size_t size, int result) {
  char steps[H2_LOADER_MFG_STEP_MAX + 1];
  for (uint32_t i = 0; i < m->model.summary.total; ++i)
    steps[i] = (char)('0' + m->model.summary.step_status[i]);
  steps[m->model.summary.total] = '\0';
  const char* name = m->initialized && m->model.current < m->profile->total
                         ? m->profile->steps[m->model.current].name
                         : "done";
  (void)snprintf(
      out, size,
      "H2_MFG_STATUS board=%s active=%d initialized=%d step=%s index=%" PRIu32
      " total=%" PRIu32 " ticket=%08" PRIx32
      " ready=%d error=%d touch_open=%d"
      " complete=%d steps=%s result=%s code=%d detail=\"%s\"",
      m->profile->board, m->running && !m->stopping, m->initialized, name,
      m->model.current, m->profile->total, m->model.ticket, m->model.ready,
      m->model.error, m->touch_open,
      m->initialized && h2_mfg_model_complete(&m->model), steps,
      result == 0 ? "OK" : "fail", result, m->detail);
}

int h2_mfg_create(h2_runtime_t* rt, const h2_mfg_profile_t* profile,
                  h2_mfg_t** out) {
  if (rt == NULL || profile == NULL || out == NULL || profile->board == NULL ||
      rt->board == NULL || strcmp(rt->board, profile->board) != 0 ||
      profile->total == 0 || profile->total > H2_LOADER_MFG_STEP_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  *out = NULL;
  h2_mfg_t* m = h2_pal_mem_alloc(rt->mem, sizeof(*m));
  if (m == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(m, 0, sizeof(*m));
  m->rt = rt;
  m->profile = profile;
  h2_pal_mutex_config_t mc = {.name = "mfg/state", .allocator = rt->mem};
  h2_pal_semaphore_config_t sc = {
      .name = "mfg/finished", .allocator = rt->mem, .max_count = 1};
  int rc = h2_pal_mutex_create(rt->sync, &mc, &m->mutex);
  if (rc == 0) rc = h2_pal_semaphore_create(rt->sync, &sc, &m->finished);
  if (rc != 0) {
    if (m->mutex != NULL) (void)h2_pal_mutex_destroy(rt->sync, m->mutex);
    h2_pal_mem_free(rt->mem, m);
    return rc;
  }
  *out = m;
  return H2_PAL_OK;
}

int h2_mfg_command(void* user, h2_loader_command_transport_t transport,
                   size_t argc, const char* const* argv, char* output,
                   size_t capacity) {
  h2_mfg_t* m = user;
  if (m == NULL || output == NULL || capacity == 0)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  if (argc == 0 || (argc == 1 && strcmp(argv[0], "status") == 0)) {
    rc = H2_PAL_OK;
  } else if (!m->running || m->stopping || !m->initialized) {
    rc = H2_PAL_ERR_INVALID_STATE;
  } else if (argc != 3 || strlen(argv[2]) != 8 ||
             strspn(argv[2], "0123456789abcdefABCDEF") != 8) {
    rc = H2_PAL_ERR_INVALID_ARG;
  } else {
    uint32_t ticket = (uint32_t)strtoul(argv[2], NULL, 16);
    if (strcmp(argv[0], "probe") == 0) {
      /* A serial command cannot manufacture proof of a real BLE connection. */
      if (transport != H2_LOADER_COMMAND_TRANSPORT_BLE ||
          m->model.current >= m->profile->total ||
          m->profile->steps[m->model.current].check != H2_MFG_BLE ||
          strcmp(argv[1], m->profile->steps[m->model.current].name) != 0 ||
          ticket != m->model.ticket || m->hardware_ticket != ticket) {
        rc = H2_PAL_ERR_INVALID_STATE;
      } else {
        m->ble_probed = 1;
        m->model.ready = 1;
        (void)snprintf(m->detail, sizeof(m->detail), "BLE command round trip");
        rc = H2_PAL_OK;
      }
    } else {
      rc = h2_mfg_model_action(&m->model, argv[0], argv[1], ticket);
    }
  }
  status(m, output, capacity, rc);
  int unlock_rc = h2_pal_mutex_unlock(m->rt->sync, m->mutex);
  return rc == 0 ? unlock_rc : rc;
}

int h2_mfg_before_disruptive(void* user, h2_loader_disruptive_action_t action) {
  h2_mfg_t* m = user;
  (void)action;
  if (m == NULL) return H2_PAL_ERR_INVALID_ARG;
  int rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  m->stopping = 1;
  int running = m->running;
  if (!running && m->cleanup_result != 0)
    m->cleanup_result = h2_mfg_hardware_close(m);
  int cleanup = m->cleanup_result;
  (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
  if (!running) return cleanup;
  /* run() never blocks on the operation mutex, so teardown cannot deadlock
   * with the command that owns that mutex while waiting here. */
  rc = h2_pal_semaphore_take(m->rt->sync, m->finished, 10000);
  if (rc != 0) return rc;
  rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  cleanup = m->cleanup_result;
  (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
  return cleanup;
}

int h2_mfg_destroy(h2_mfg_t** session) {
  if (session == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_mfg_t* m = *session;
  if (m == NULL) return 0;
  int rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  rc = m->running ? H2_PAL_ERR_BUSY : h2_mfg_hardware_close(m);
  (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  rc = m->finished == NULL ? 0
                           : h2_pal_semaphore_destroy(m->rt->sync, m->finished);
  if (rc != 0) return rc;
  m->finished = NULL;
  rc = h2_pal_mutex_destroy(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  h2_pal_mem_free(m->rt->mem, m);
  *session = NULL;
  return 0;
}

void h2_mfg_rearm(void* user) {
  h2_mfg_t* m = user;
  if (h2_pal_mutex_lock(m->rt->sync, m->mutex) != 0) return;
  if (!m->running && m->cleanup_result == 0) m->stopping = 0;
  (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
}

int h2_mfg_run(void* user, h2_loader_t* loader,
               const h2_pal_sync_api_t* operation_sync,
               h2_pal_mutex_t* operation_mutex,
               const h2_pal_sync_api_t* wifi_sync, h2_pal_mutex_t* wifi_mutex) {
  h2_mfg_t* m = user;
  (void)wifi_sync;
  (void)wifi_mutex;
  if (m == NULL || loader == NULL || operation_sync == NULL ||
      operation_mutex == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  int rc = H2_PAL_OK;
  int bypass = 0;
  int recovery = 0;
  int opened = 0;
  rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
  if (rc != 0) return rc;
  (void)h2_pal_semaphore_take(m->rt->sync, m->finished, H2_PAL_SYNC_NO_WAIT);
  m->loader = loader;
  (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
  for (;;) {
    /* Stop is checked before trying operation_mutex. No blocking acquisition
     * is allowed: before_disruptive may own it while it joins this session. */
    rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
    if (rc != 0) return rc;
    if (m->stopping) {
      (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
      break;
    }
    m->running = 1;
    (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
    rc = h2_pal_mutex_try_lock(operation_sync, operation_mutex);
    if (rc == H2_PAL_ERR_WOULD_BLOCK || rc == H2_PAL_ERR_BUSY) {
      (void)h2_pal_time_sleep_ms(m->rt->time, 20);
      continue;
    }
    if (rc != 0) break;
    rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
    if (rc != 0) {
      (void)h2_pal_mutex_unlock(operation_sync, operation_mutex);
      break;
    }
    if (!opened) {
      h2_loader_status_t ls;
      rc = h2_loader_read_status(loader, &ls);
      /* Complete self-update/recovery before any UI, gate revision reset or
       * peripheral work. AUTO App startup remains subject to Loader's gate. */
      recovery =
          rc == 0 &&
          (ls.running_partition_id != loader->config.h2loader_partition_id ||
           (ls.boot_intent == H2_LOADER_BOOT_INTENT_AUTO && ls.stage.valid &&
            ls.stage.role == H2_LOADER_IMAGE_ROLE_H2LOADER));
      bypass = recovery;
      if (rc == 0 && !recovery) {
        if (m->profile->append_from_total != 0 ||
            m->profile->append_from_revision != 0)
          rc = h2_loader_mfg_ensure_appended_revision(
              m->rt->pref, m->profile->total, m->profile->revision,
              m->profile->append_from_total, m->profile->append_from_revision);
        else
          rc = h2_loader_mfg_ensure_acceptance_revision(
              m->rt->pref, m->profile->total, m->profile->revision);
      }
      h2_loader_mfg_summary_t summary;
      int present = 0;
      uint32_t ticket = 0;
      if (rc == 0 && !recovery)
        rc = h2_loader_mfg_read(m->rt->pref, m->rt->mem, &summary, &present);
      if (rc == 0 && !recovery)
        rc = h2_pal_crypto_random(m->rt->crypto, (uint8_t*)&ticket,
                                  sizeof(ticket));
      if (rc == 0 && !recovery)
        rc = h2_mfg_model_init(&m->model, m->profile, &summary,
                               ticket == 0 ? 1 : ticket, save, m);
      if (rc == 0 && !recovery) {
        loader->status.mfg = summary;
        m->initialized = 1;
        m->hardware_ticket = 0;
        opened = 1;
        char line[192];
        (void)snprintf(
            line, sizeof(line),
            "H2_MFG_BEGIN board=%s revision=%" PRIu32 " total=%" PRIu32,
            m->profile->board, m->profile->revision, m->profile->total);
        (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg", line);
        int app_work =
            (ls.stage.valid && ls.stage.role == H2_LOADER_IMAGE_ROLE_APP) ||
            (ls.partition_2.valid &&
             ls.partition_2.role == H2_LOADER_IMAGE_ROLE_APP);
        bypass = ls.boot_intent == H2_LOADER_BOOT_INTENT_AUTO && app_work &&
                 h2_mfg_model_complete(&m->model);
      }
    }
    const char* action = NULL;
    uint64_t now = 0;
    if (rc == 0 && !bypass) drain_events(m);
    if (rc == 0 && !bypass)
      rc = h2_pal_time_get_monotonic_ms(m->rt->time, &now);
    /* Hardware ownership stays on this task. Commands change only the model;
     * a new ticket always tears down the previous step before probing again. */
    if (rc == 0 && !bypass && m->hardware_ticket != m->model.ticket) {
      rc = h2_mfg_hardware_leave(m);
      if (rc == 0) {
        m->hardware_ticket = m->model.ticket;
        m->model.error = h2_mfg_hardware_enter(m, now);
        if (m->model.error != 0) {
          int error = m->model.error;
          int cleanup = h2_mfg_hardware_leave(m);
          int save_rc = m->model.current < m->profile->total
                            ? h2_mfg_model_action(
                                  &m->model, "fail",
                                  m->profile->steps[m->model.current].name,
                                  m->model.ticket)
                            : error;
          /* The failed check stays selected, without repeatedly reopening it.
           */
          m->hardware_ticket = m->model.ticket;
          m->model.error = error;
          if (save_rc != 0) rc = save_rc;
          if (rc == 0) rc = cleanup;
        }
      }
    }
    if (rc == 0 && !bypass) {
      int check_rc = h2_mfg_hardware_tick(m, now, &action);
      if (check_rc != 0) {
        int cleanup = h2_mfg_hardware_leave(m);
        const char* step = m->profile->steps[m->model.current].name;
        rc = h2_mfg_model_action(&m->model, "fail", step, m->model.ticket);
        m->hardware_ticket = m->model.ticket;
        m->model.error = check_rc;
        if (rc == 0) rc = cleanup;
      }
    }
    if (rc == 0 && !bypass && action != NULL &&
        m->model.current < m->profile->total) {
      rc = h2_mfg_model_action(&m->model, action,
                               m->profile->steps[m->model.current].name,
                               m->model.ticket);
      /* Rejected premature local confirmation leaves the session usable. */
      if (rc == H2_PAL_ERR_INVALID_STATE) rc = 0;
    }
    if (rc == 0 && !bypass && now - m->last_draw_ms >= 100) {
      m->last_draw_ms = now;
      rc = h2_mfg_ui_draw(m, now);
    }
    if (!bypass && now - m->last_log_ms >= 1000) {
      char line[240];
      m->last_log_ms = now;
      const char* name = m->model.current < m->profile->total
                             ? m->profile->steps[m->model.current].name
                             : "done";
      (void)snprintf(line, sizeof(line),
                     "H2_MFG_STEP step=%s ticket=%08" PRIx32
                     " ready=%d error=%d complete=%d code=%d",
                     name, m->model.ticket, m->model.ready, m->model.error,
                     h2_mfg_model_complete(&m->model), rc);
      (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg", line);
      (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg", m->detail);
    }
    (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
    (void)h2_pal_mutex_unlock(operation_sync, operation_mutex);
    if (rc != 0 || bypass) break;
    (void)h2_pal_time_sleep_ms(m->rt->time, 20);
  }
  int lock_rc = h2_pal_mutex_lock(m->rt->sync, m->mutex);
  if (lock_rc != 0) return lock_rc;
  m->cleanup_result = h2_mfg_hardware_close(m);
  if (rc == 0) rc = m->cleanup_result;
  m->running = 0;
  (void)h2_pal_semaphore_give(m->rt->sync, m->finished);
  (void)h2_pal_mutex_unlock(m->rt->sync, m->mutex);
  char line[128];
  (void)snprintf(line, sizeof(line),
                 "H2_MFG_STOP recovery=%d cleanup=%d code=%d", bypass,
                 m->cleanup_result, rc);
  (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_INFO, "mfg", line);
  return rc != 0 ? rc : bypass && !recovery ? H2_PAL_OK : H2_PAL_EXIT;
}
