#ifndef H2_JIELI_WL82_CONSOLE_H
#define H2_JIELI_WL82_CONSOLE_H

#include "h2/pal/os/h2_pal_io_stream.h"

const h2_pal_io_stream_api_t* h2_jieli_wl82_platform_io_stream_api(void);
#include <stddef.h>
#include <stdint.h>

/* Board wiring, borrowed for the entire boot. The native provider owns CDC
 * tasks/semaphores/ring storage and TX serialization. No framing/session types
 * cross this boundary. UART callbacks use the board's DMA-safe console. */
typedef struct h2_jieli_wl82_console_config {
  int (*start)(void);
  int (*read)(void* buffer, size_t len);
  int (*write)(const void* buffer, size_t len, uint32_t timeout_ms);
} h2_jieli_wl82_console_config_t;
const h2_jieli_wl82_console_config_t* h2_jieli_wl82_console_board_config(void);

/* Idempotent. Failed higher-level worker creation never destroys live RX
 * storage; a retry borrows the same native physical console. */
int h2_jieli_wl82_console_start(void);
h2_pal_result_t h2_jieli_wl82_console_read(void* user, void* buffer, size_t len,
                                           size_t* out_read,
                                           uint32_t timeout_ms);
h2_pal_result_t h2_jieli_wl82_console_write(void* user, const void* buffer,
                                            size_t len, size_t* out_written,
                                            uint32_t timeout_ms);
h2_pal_result_t h2_jieli_wl82_console_flush(void* user);
void h2_jieli_wl82_diag_write(const char* text);

/* Retained native capture is persisted in normal worker context. H2 policy
 * decides whether a crash disables optional transports or automatic launch. */
int h2_jieli_wl82_take_loader_crash_pending(void);
int h2_jieli_wl82_coredump_flush_pending(void);

#endif
