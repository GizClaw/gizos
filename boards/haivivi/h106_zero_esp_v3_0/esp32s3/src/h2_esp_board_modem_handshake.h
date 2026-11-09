#ifndef H2_ESP_BOARD_MODEM_HANDSHAKE_H
#define H2_ESP_BOARD_MODEM_HANDSHAKE_H

#include <stdbool.h>

/* Board-private open policy. Adapters retain UART, deadlines and diagnostics.
 * Every callback returns zero on success; aborted distinguishes an exhausted
 * open budget from a completed silent probe or a timed-out AT command. */
typedef struct {
  int (*probe_high)(void* user, unsigned attempts);
  void (*send_cld)(void* user);
  int (*probe_low)(void* user);
  bool (*aborted)(void* user);
  void (*report)(void* user, bool cld, int rc);
  int (*power_cycle)(void* user);
  void (*fresh)(void* user, bool cold, bool recovery);
  int (*qsclk)(void* user);
} h2_modem_handshake_ops_t;

static inline int h2_modem_handshake_open(const h2_modem_handshake_ops_t* ops,
                                          void* user, int timeout_error) {
  int rc = ops->probe_high(user, 3u);
  if (ops->aborted(user)) return rc;
  if (rc != 0) {
    ops->send_cld(user);
    rc = ops->probe_high(user, 2u);
    if (ops->aborted(user)) return rc;
    ops->report(user, true, rc);
  }
  const bool high = rc == 0;
  if (!high) {
    rc = ops->probe_low(user);
    if (ops->aborted(user)) return rc;
  }
  ops->report(user, false, rc);
  bool cycled = false;
  if (rc != 0) {
    rc = ops->power_cycle(user);
    if (rc != 0) return rc;
    cycled = true;
  }
  ops->fresh(user, !high || cycled, false);
  rc = ops->qsclk(user);
  if (ops->aborted(user)) return rc;
  if (rc == timeout_error && !cycled) {
    rc = ops->power_cycle(user);
    if (rc != 0) return rc;
    ops->fresh(user, true, true);
    /* No loop: a second QSCLK failure must fail this open. */
    rc = ops->qsclk(user);
  }
  return rc;
}

/* Cut the rail once, then cold-handshake. Always attempt off on failure. */
typedef struct {
  int (*power)(void* user, int level);
  void (*wait_ms)(unsigned ms);
  int (*cold_ready)(void* user);
  void (*report)(void* user, int rc);
} h2_modem_power_cycle_ops_t;

static inline int h2_modem_handshake_power_cycle(
    const h2_modem_power_cycle_ops_t* ops, void* user) {
  int rc = ops->power(user, 0);
  if (rc == 0) {
    ops->wait_ms(1200u);
    rc = ops->power(user, 1);
  }
  if (rc == 0) rc = ops->cold_ready(user);
  ops->report(user, rc);
  if (rc != 0) (void)ops->power(user, 0);
  return rc;
}
#endif
