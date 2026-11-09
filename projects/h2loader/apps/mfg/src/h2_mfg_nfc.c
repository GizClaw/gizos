#include "h2_mfg_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int active(const h2_mfg_t* m) {
  return m->nfc.active && m->nfc.ticket == m->model.ticket &&
         m->hardware_ticket == m->model.ticket &&
         m->model.current < m->profile->total &&
         m->profile->steps[m->model.current].check == H2_MFG_NFC;
}

static int validate(const h2_runtime_nfc_state_t* s) {
  if (s->result != 0) return s->result;
  if (s->uid_len > sizeof(s->uid)) return H2_PAL_ERR_FORMAT;
  switch (s->status) {
    case H2_RUNTIME_NFC_STATE_NONE:
      return s->uid_len == 0 && s->stage == H2_PAL_NFC_STAGE_ABSENT &&
                     s->tag_type == H2_PAL_NFC_TAG_TYPE_UNKNOWN
                 ? 0
                 : H2_PAL_ERR_FORMAT;
    case H2_RUNTIME_NFC_STATE_ABSENT:
      return s->stage == H2_PAL_NFC_STAGE_ABSENT && s->uid_len == 0 &&
                     s->tag_type == H2_PAL_NFC_TAG_TYPE_UNKNOWN
                 ? 0
                 : H2_PAL_ERR_FORMAT;
    case H2_RUNTIME_NFC_STATE_DISCOVERED:
      return s->stage == H2_PAL_NFC_STAGE_DISCOVERED && s->uid_len != 0 &&
                     s->tag_type >= H2_PAL_NFC_TAG_TYPE_ISO14443A &&
                     s->tag_type <= H2_PAL_NFC_TAG_TYPE_NTAG
                 ? 0
                 : H2_PAL_ERR_FORMAT;
    case H2_RUNTIME_NFC_STATE_ERROR:
      return H2_PAL_ERR_IO;
    default:
      return H2_PAL_ERR_FORMAT;
  }
}

static int fault(h2_mfg_t* m, int rc) {
  if (m->nfc.error == 0) {
    h2_runtime_nfc_state_t state = {0};
    h2_runtime_input_status_t health = {0};
    uint32_t dropped = 0;
    const int state_rc =
        h2_runtime_component_state_nfc(m->rt, H2_MFG_NFC_COMPONENT_ID, &state);
    const int health_rc = h2_runtime_input_status(m->rt, &health);
    const int dropped_rc = h2_runtime_dropped_event_count(m->rt, &dropped);
    char diagnostic[256];
    (void)snprintf(
        diagnostic, sizeof(diagnostic),
        "H2_MFG_NFC_FAULT code=%d state_rc=%d state=%u result=%d "
        "health_rc=%d phase=%u worker=%d dropped_rc=%d baseline=%" PRIu32
        " dropped=%" PRIu32,
        rc, state_rc, (unsigned)state.status, state.result, health_rc,
        (unsigned)health.phase, health.worker_result, dropped_rc,
        m->nfc.dropped, dropped);
    (void)h2_pal_log_write(m->rt->log, H2_PAL_LOG_ERROR, "mfg", diagnostic);
    m->nfc.error = rc;
    /* Preserve the first fault even if acquisition later recovers. The status
     * command must distinguish a reader error from lost Runtime events. */
    (void)snprintf(m->detail, sizeof(m->detail),
                   "NFC error=%d state_rc=%d state=%u result=%d "
                   "dropped=%" PRIu32 "/%" PRIu32 "; retry required",
                   rc, state_rc, (unsigned)state.status, state.result,
                   m->nfc.dropped, dropped);
  }
  m->nfc.evidence = m->model.ready = 0;
  return m->nfc.error;
}

static void observe(h2_mfg_t* m, const h2_runtime_nfc_state_t* s) {
  if (s->updated_at_ms <= m->nfc.baseline_ms ||
      s->updated_at_ms <= m->nfc.latest_ms)
    return;
  int rc = validate(s);
  if (rc != 0) {
    (void)fault(m, rc);
    return;
  }
  m->nfc.latest_ms = s->updated_at_ms;
  if (s->status == H2_RUNTIME_NFC_STATE_ABSENT) {
    m->nfc.armed = 1;
    m->nfc.evidence = 0;
  } else if (s->status == H2_RUNTIME_NFC_STATE_DISCOVERED && m->nfc.armed) {
    m->nfc.tag = *s;
    m->nfc.evidence = 1;
  }
}

int h2_mfg_nfc_enter(h2_mfg_t* m) {
  memset(&m->nfc, 0, sizeof(m->nfc));
  m->nfc.active = 1;
  m->nfc.ticket = m->model.ticket;
  m->nfc.baseline_ms = m->started_ms;
  h2_runtime_nfc_state_t state;
  int rc =
      h2_runtime_component_state_nfc(m->rt, H2_MFG_NFC_COMPONENT_ID, &state);
  if (rc == 0) rc = validate(&state);
  if (rc != 0) return fault(m, rc);
  if (state.updated_at_ms > m->nfc.baseline_ms)
    m->nfc.baseline_ms = state.updated_at_ms;
  m->nfc.latest_ms = m->nfc.baseline_ms;
  /* Unchanged empty scans are suppressed by Runtime. The baseline itself is
   * sufficient removal evidence, but a retained tag is never sufficient. */
  m->nfc.armed = state.status == H2_RUNTIME_NFC_STATE_ABSENT;
  rc = h2_runtime_dropped_event_count(m->rt, &m->nfc.dropped);
  if (rc != 0) return fault(m, rc);
  /* The session commits hardware ownership after preparation/enter succeeds.
   * Its first controller tick revalidates health with that committed ticket. */
  return m->hardware_ticket == m->model.ticket ? h2_mfg_nfc_tick(m) : 0;
}

void h2_mfg_nfc_event(h2_mfg_t* m, const h2_runtime_event_t* event) {
  if (!active(m) || m->nfc.error ||
      event->component != H2_RUNTIME_COMPONENT_NFC_READER ||
      event->component_id != H2_MFG_NFC_COMPONENT_ID ||
      event->timestamp_ms <= m->nfc.baseline_ms ||
      event->sequence <= m->nfc.sequence)
    return;
  if (event->kind != H2_RUNTIME_COMPONENT_EVENT_NFC_STATE &&
      event->kind != H2_RUNTIME_COMPONENT_EVENT_ERROR)
    return;
  m->nfc.sequence = event->sequence;
  if (event->kind == H2_RUNTIME_COMPONENT_EVENT_ERROR) {
    (void)fault(m, H2_PAL_ERR_IO);
    return;
  }
  if (event->payload == NULL ||
      event->payload_size != sizeof(h2_runtime_nfc_state_t) ||
      event->payload_capacity < event->payload_size) {
    (void)fault(m, H2_PAL_ERR_FORMAT);
    return;
  }
  h2_runtime_nfc_state_t state;
  memcpy(&state, event->payload, sizeof(state));
  if (state.updated_at_ms != event->timestamp_ms) {
    (void)fault(m, H2_PAL_ERR_FORMAT);
    return;
  }
  int rc = validate(&state);
  if (rc != 0)
    (void)fault(m, rc);
  else
    observe(m, &state);
}

int h2_mfg_nfc_tick(h2_mfg_t* m) {
  m->model.ready = 0;
  if (!active(m)) return H2_PAL_ERR_INVALID_STATE;
  if (m->nfc.error) return fault(m, m->nfc.error);
  h2_runtime_input_status_t health;
  int health_rc = h2_runtime_input_status(m->rt, &health);
  if (health_rc != 0) return fault(m, health_rc);
  if (health.worker_result != 0) return fault(m, health.worker_result);
  if (health.phase != H2_RUNTIME_INPUT_PHASE_TASK_RUNNING)
    return fault(m, H2_PAL_ERR_INVALID_STATE);
  h2_runtime_nfc_state_t state;
  int rc =
      h2_runtime_component_state_nfc(m->rt, H2_MFG_NFC_COMPONENT_ID, &state);
  if (rc == 0) rc = validate(&state);
  if (rc != 0) return fault(m, rc);
  uint32_t dropped = 0;
  rc = h2_runtime_dropped_event_count(m->rt, &dropped);
  if (rc != 0) return fault(m, rc);
  /* A lost intermediate error cannot be ruled out by a healthy latest snapshot.
   * Conservatively require retry after any Runtime producer loss this attempt.
   */
  if (dropped != m->nfc.dropped) return fault(m, H2_PAL_ERR_IO);
  observe(m, &state);
  if (m->nfc.error) return fault(m, m->nfc.error);
  m->model.ready = !m->runtime_events_pending && m->nfc.evidence &&
                   state.status == H2_RUNTIME_NFC_STATE_DISCOVERED &&
                   state.updated_at_ms == m->nfc.tag.updated_at_ms &&
                   state.tag_type == m->nfc.tag.tag_type &&
                   state.uid_len == m->nfc.tag.uid_len &&
                   memcmp(state.uid, m->nfc.tag.uid, state.uid_len) == 0;
  (void)snprintf(m->detail, sizeof(m->detail),
                 "NFC %s type=%u uid_bytes=%u updated_ms=%" PRIu64,
                 m->model.ready ? "tag verified; confirm"
                 : m->nfc.armed ? "present a tag"
                                : "remove tag then present",
                 (unsigned)state.tag_type, (unsigned)state.uid_len,
                 (uint64_t)state.updated_at_ms);
  return 0;
}
