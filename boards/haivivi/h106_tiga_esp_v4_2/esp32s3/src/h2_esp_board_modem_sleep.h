#ifndef H2_ESP_BOARD_MODEM_SLEEP_H
#define H2_ESP_BOARD_MODEM_SLEEP_H

#include "h2_atomic.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  H2_TIGA_MODEM_SLEEP_PENDING,
  H2_TIGA_MODEM_SLEEP_READY,
  H2_TIGA_MODEM_SLEEP_TERMINAL,
};

/* Successful supervised preparation seals the modem session. A late AT,
 * DTR or power request must not revoke the receipt and cause a power cut. */
static inline bool h2_tiga_modem_sleep_admit(h2_atomic_int_t* state) {
  return h2_atomic_load(state) == H2_TIGA_MODEM_SLEEP_PENDING;
}

/* Only the preparation owner revokes a receipt after a real cleanup error.
 * Terminal admission cannot be undone by a late worker completion. */
static inline bool h2_tiga_modem_sleep_invalidate(h2_atomic_int_t* state) {
  int previous = h2_atomic_load(state);
  while (previous != H2_TIGA_MODEM_SLEEP_TERMINAL) {
    if (h2_atomic_compare_exchange_strong(state, &previous,
                                          H2_TIGA_MODEM_SLEEP_PENDING))
      return true;
  }
  return false;
}

static inline bool h2_tiga_modem_sleep_ready(h2_atomic_int_t* state) {
  int expected = H2_TIGA_MODEM_SLEEP_PENDING;
  return h2_atomic_compare_exchange_strong(state, &expected,
                                           H2_TIGA_MODEM_SLEEP_READY);
}

/* An ordinary PAL attempt must not leave a receipt that a later supervisor
 * could mistake for preparation belonging to its own terminal transition. */
static inline bool h2_tiga_modem_sleep_complete(h2_atomic_int_t* state,
                                                bool supervised) {
  return supervised ? h2_tiga_modem_sleep_ready(state)
                    : h2_atomic_load(state) != H2_TIGA_MODEM_SLEEP_TERMINAL;
}

static inline bool h2_tiga_modem_sleep_commit(h2_atomic_int_t* state) {
  return h2_atomic_exchange(state, H2_TIGA_MODEM_SLEEP_TERMINAL) ==
         H2_TIGA_MODEM_SLEEP_READY;
}

/* A passive MCU wake has made no modem transaction. Preserve its inherited
 * rail/configuration without demanding a new AT preparation. Once a session
 * starts, the usual current-preparation receipt remains mandatory. */
static inline bool h2_tiga_modem_sleep_commit_session(h2_atomic_int_t* state,
                                                      bool accessed) {
  const int previous = h2_atomic_exchange(state, H2_TIGA_MODEM_SLEEP_TERMINAL);
  return previous == H2_TIGA_MODEM_SLEEP_READY ||
         (!accessed && previous == H2_TIGA_MODEM_SLEEP_PENDING);
}

/* Output-only pads cannot be observed through their disabled input after a
 * software reset. Keep the last successfully driven rail across retained
 * resets; a cold reset must never trust RTC_NOINIT contents. */
static inline void h2_tiga_modem_power_record(uint32_t* marker, bool powered) {
  *marker = powered ? 0x48504f4eu : 0x48504f46u;
}

static inline bool h2_tiga_modem_power_restore(uint32_t marker, bool retained,
                                               bool* powered) {
  if (!retained || (marker != 0x48504f4eu && marker != 0x48504f46u))
    return false;
  *powered = marker == 0x48504f4eu;
  return true;
}

/* RTC_NOINIT marker: consumed once, and only meaningful after deep sleep. */
static inline void h2_tiga_modem_sleep_fallback_set(uint32_t* marker,
                                                    bool fallback) {
  *marker = fallback ? 0x48534c50u : 0u;
}

static inline bool h2_tiga_modem_sleep_fallback_take(uint32_t* marker,
                                                     bool deep_sleep_reset) {
  const bool fallback = deep_sleep_reset && *marker == 0x48534c50u;
  *marker = 0u;
  return fallback;
}

/* Keep the AT DLCI fixed. SDK enter_data swaps only after successful dial;
 * on failure restore the local terminal selection, leaving CMUX intact. */
typedef struct {
  int (*swap)(void);
  int (*enter)(void);
} h2_tiga_modem_data_ops_t;

static inline int h2_tiga_modem_data_start(
    const h2_tiga_modem_data_ops_t* ops) {
  int rc = ops->swap();
  if (rc != 0) return rc;
  rc = ops->enter();
  if (rc == 0) return 0;
  const int restore_rc = ops->swap();
  return restore_rc != 0 ? restore_rc : rc;
}

/* Inverted DTR gate. The adapter owns GPIO retention, AT and task waits. */
typedef struct {
  int (*drive)(int level);
  int (*drain)(void);
  int (*ready)(void);
  void (*wait_ms)(unsigned ms);
} h2_tiga_modem_dtr_ops_t;

static inline int h2_tiga_modem_dtr_gate(const h2_tiga_modem_dtr_ops_t* ops,
                                         int allow_sleep) {
  if (allow_sleep) {
    int rc = ops->drain();
    return rc == 0 ? ops->drive(0) : rc;
  }
  int rc = ops->drive(1);
  if (rc != 0) return rc;
  /* Conservative board settle; the EC25 manual specifies no fixed latency. */
  ops->wait_ms(500u);
  for (unsigned attempt = 0; attempt < 10u; ++attempt) {
    rc = ops->ready();
    if (rc == 0) return 0;
    if (attempt + 1u < 10u) ops->wait_ms(100u);
  }
  return rc;
}

/* Terminal board sequence; each step returns zero on success. Command waits
 * live here in prepare, never in the supervisor's GPIO-only commit. */
typedef struct {
  int (*close_ppp)(void);
  int (*exit_cmux)(void);
  int (*ri_wake)(void);
  int (*quiet_urcs)(const char* command);
  int (*qsclk)(const char* command);
  int (*drain_tx)(void);
} h2_tiga_modem_sleep_ops_t;

static inline int h2_tiga_modem_sleep_prepare(
    const h2_tiga_modem_sleep_ops_t* ops) {
  int rc = ops->close_ppp();
  const int cmux_rc = ops->exit_cmux();
  if (rc == 0) rc = cmux_rc;
  const int ri_rc = ops->ri_wake();
  if (rc == 0) rc = ri_rc;
  /* Keep SIM and incoming-call RI notifications active in standby. */
  (void)ops->quiet_urcs("AT+QCFG=\"urc/ri/other\",\"pulse\",120,1");
  (void)ops->quiet_urcs("AT+QSIMSTAT=1");
  /* QSCLK is idempotent. Retry once inside the existing shutdown deadline;
   * failure invalidates retention, but the MCU can still sleep with 4G off. */
  int qsclk_rc = ops->qsclk("AT+QSCLK=1");
  if (qsclk_rc != 0) qsclk_rc = ops->qsclk("AT+QSCLK=1");
  if (rc == 0) rc = qsclk_rc;
  const int drain_rc = ops->drain_tx();
  return rc != 0 ? rc : drain_rc;
}
#endif
