/* Temporary, opt-in WL82 SDK ABI diagnostic, not a PAL contract.
 * Layouts verified from the current btctrler archive's link_layer.c.o and
 * RF_ble.c.o LLVM IR. Capture only, never printf/allocate in RF callbacks.
 * Remove before production delivery or any SDK revision change. */
#ifdef H2_JIELI_BLE_DIAG_LL_CAPTURE
typedef struct h2_ll_diag_handler {
  void (*rx)(void *, void *);
  void (*rx_post)(void *, void *);
  void (*tx)(void *, void *);
  void (*event)(void *, int, int);
  void (*unused)(void);
  void (*echo)(void *, int, void *, uint16_t);
} h2_ll_diag_handler_t;
typedef struct h2_ll_diag_ops {
  void (*before[5])(void);
  int (*advertising)(void *, const void *);
  void (*middle[4])(void);
  void (*handler_register)(void *, void *, const h2_ll_diag_handler_t *);
  void (*after[6])(void);
} h2_ll_diag_ops_t;
extern const h2_ll_diag_ops_t *__ble_ops;
extern const h2_ll_diag_handler_t conn_handler;
_Static_assert(sizeof(h2_ll_diag_handler_t) == 24, "WL82 handler ABI");
_Static_assert(sizeof(h2_ll_diag_ops_t) == 68, "WL82 operations ABI");
static const h2_ll_diag_ops_t *h2_ll_original_ops;
static h2_ll_diag_ops_t h2_ll_capture_ops;
static h2_ll_diag_handler_t h2_ll_capture_handler;
static volatile const uint16_t *h2_ll_param_shadow;
static uint16_t h2_ll_adv_header[2];
static int h2_ll_adv_result;
static unsigned h2_ll_adv_calls;
static int h2_ll_capture_advertising(void *hw, const void *adv) {
  int rc = h2_ll_original_ops->advertising(hw, adv);
  volatile const uint16_t *shadow = *(volatile const uint16_t **)hw;
  h2_ll_adv_header[0] = shadow[11];
  h2_ll_adv_header[1] = shadow[12];
  h2_ll_adv_result = rc;
  ++h2_ll_adv_calls;
  return rc;
}
static struct {
  uint8_t params[22];
  uint8_t peer[6];
  volatile unsigned rx, tx, events;
  uint16_t event_type[24], event_number[24];
  uint16_t rx_flags[24], rx_toggle[24], rx_buffers[24];
  uint32_t registered_ms, event_ms[24], window_us[24];
  uint32_t initial_window_us;
  uint16_t initial_extra_us, initial_widen[2];
} h2_ll_trace;
static void h2_ll_capture_rx(void *priv, void *rx) {
  ++h2_ll_trace.rx;
  conn_handler.rx(priv, rx);
}
static void h2_ll_capture_tx(void *priv, void *tx) {
  ++h2_ll_trace.tx;
  conn_handler.tx(priv, tx);
}
static void h2_ll_capture_event(void *priv, int type, int param) {
  unsigned n = h2_ll_trace.events;
  if (n < 24u) {
    h2_ll_trace.event_ms[n] = timer_get_ms();
    /* Current RF_ble.c.o: ble_param is 324 bytes; ble4_hw field 3
     * is the flags halfword at byte 326. RX IRQ sets bit 0 before
     * buffer processing. Event callback precedes autozoom clearing it.
     * RXBUF0/1CNTL are bytes 102/103 in the DMA parameter shadow. */
    h2_ll_trace.rx_flags[n] = h2_ll_param_shadow[163];
    h2_ll_trace.rx_toggle[n] = h2_ll_param_shadow[8];
    h2_ll_trace.rx_buffers[n] = h2_ll_param_shadow[51];
    h2_ll_trace.window_us[n] = (uint32_t)h2_ll_param_shadow[17] |
        ((uint32_t)h2_ll_param_shadow[18] << 16u);
    h2_ll_trace.event_type[n] = (uint16_t)type;
    h2_ll_trace.event_number[n] = (uint16_t)param;
  }
  h2_ll_trace.events = n + 1u;
  conn_handler.event(priv, type, param);
}
static void h2_ll_capture_register(
    void *hw, void *priv, const h2_ll_diag_handler_t *handler) {
  if (handler == &conn_handler && priv != NULL) {
    memset(&h2_ll_trace, 0, sizeof(h2_ll_trace));
    memcpy(h2_ll_trace.params, (const uint8_t *)priv + 20u, 22u);
    memcpy(h2_ll_trace.peer, (const uint8_t *)priv + 12u, 6u);
    /* RF_ble.c.o: ble_hw[0] points to ble4_hw, which begins with the
     * DMA parameter shadow. No indexed MMIO access or register writes. */
    h2_ll_param_shadow = *(volatile const uint16_t **)hw;
    h2_ll_trace.registered_ms = timer_get_ms();
    h2_ll_trace.initial_window_us = (uint32_t)h2_ll_param_shadow[17] |
        ((uint32_t)h2_ll_param_shadow[18] << 16u);
    h2_ll_trace.initial_extra_us = h2_ll_param_shadow[137];
    h2_ll_trace.initial_widen[0] = h2_ll_param_shadow[36];
    h2_ll_trace.initial_widen[1] = h2_ll_param_shadow[37];
    h2_ll_original_ops->handler_register(hw, priv, &h2_ll_capture_handler);
  } else {
    h2_ll_original_ops->handler_register(hw, priv, handler);
  }
}
static void h2_ll_diag_install(void) {
  if (h2_ll_original_ops != NULL) return;
  if (__ble_ops == NULL || __ble_ops->handler_register == NULL ||
      conn_handler.rx == NULL || conn_handler.tx == NULL ||
      conn_handler.event == NULL) {
    printf("H2_LL_DIAG install=unavailable\r\n");
    return;
  }
  h2_ll_original_ops = __ble_ops;
  h2_ll_capture_ops = *__ble_ops;
  h2_ll_capture_handler = conn_handler;
  h2_ll_capture_handler.rx = h2_ll_capture_rx;
  h2_ll_capture_handler.tx = h2_ll_capture_tx;
  h2_ll_capture_handler.event = h2_ll_capture_event;
  h2_ll_capture_ops.handler_register = h2_ll_capture_register;
  h2_ll_capture_ops.advertising = h2_ll_capture_advertising;
  __ble_ops = &h2_ll_capture_ops;
  printf("H2_LL_DIAG install=ok\r\n");
}
static void h2_ll_diag_dump(void) {
  printf("H2_LL_DIAG adv calls=%u rc=%d txahdr=%04x:%04x chsel=%u:%u\r\n",
         h2_ll_adv_calls, h2_ll_adv_result,
         h2_ll_adv_header[0], h2_ll_adv_header[1],
         (h2_ll_adv_header[0] >> 9u) & 1u,
         (h2_ll_adv_header[1] >> 9u) & 1u);
  printf("H2_LL_DIAG rx=%u tx=%u events=%u\r\n",
         h2_ll_trace.rx, h2_ll_trace.tx, h2_ll_trace.events);
  char hex[45];
  static const char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < 22u; ++i) {
    hex[i * 2u] = digits[h2_ll_trace.params[i] >> 4u];
    hex[i * 2u + 1u] = digits[h2_ll_trace.params[i] & 15u];
  }
  hex[44] = 0;
  printf("H2_LL_DIAG params=%s\r\n", hex);
  printf("H2_LL_DIAG shadow initial_window_us=%u extra_us=%u widen=%04x:%04x\r\n",
         (unsigned)h2_ll_trace.initial_window_us, h2_ll_trace.initial_extra_us,
         h2_ll_trace.initial_widen[0], h2_ll_trace.initial_widen[1]);
  const unsigned count = h2_ll_trace.events < 24u ? h2_ll_trace.events : 24u;
  for (unsigned i = 0; i < count; ++i)
    printf("H2_LL_DIAG event index=%u type=%u value=%u elapsed_ms=%u window_us=%u rxflags=%04x rxtog=%04x rxbuf=%04x\r\n", i,
           h2_ll_trace.event_type[i], h2_ll_trace.event_number[i],
           (unsigned)(h2_ll_trace.event_ms[i] - h2_ll_trace.registered_ms),
           (unsigned)h2_ll_trace.window_us[i], h2_ll_trace.rx_flags[i],
           h2_ll_trace.rx_toggle[i], h2_ll_trace.rx_buffers[i]);
}
#else
static void h2_ll_diag_install(void) {}
static void h2_ll_diag_dump(void) {}
#endif
