#include "h2_bk_h2loader_cp_transport.h"
#include "h2_bk_cp_transport_core.h"
#include "h2_bk_uart_tunnel_codec.h"

#include <stdbool.h>

#include "common/bk_include.h"
#include "bk_private/bk_uart.h"
#include "components/shell_task.h"
#include "driver/mb_uart_driver.h"
#include "driver/uart.h"
#include "os/mem.h"
#include "os/os.h"
#include "shell_drv.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(CONFIG_CLI) && CONFIG_CLI
#error "h2_cp_transport requires the CP shell CLI input path to be disabled"
#endif

#if defined(CONFIG_AT) && CONFIG_AT
#error "h2_cp_transport requires the CP AT input path to be disabled"
#endif

#define H2_BK_CP_MAILBOX_CHUNK_SIZE 128u
#define H2_BK_CP_UART_CHUNK_SIZE 128u
#define H2_BK_CP_UART_RX_QUEUE_SIZE 12288u
#define H2_BK_CP_TASK_STACK_SIZE 4096u
#define H2_BK_CP_UART_BAUD_RATE 230400u
#define H2_BK_CP_STOP_TIMEOUT_MS 1000u
#define H2_BK_CP_TX_ACK_MAGIC 0xa5u
#define H2_BK_CP_TX_ACK_VERSION 1u
#define H2_BK_CP_READY_REQUEST 0x5au
#define H2_BK_CP_READY_ACK 0x5bu

static beken_thread_t s_transport_thread;
static beken_semaphore_t s_transport_done;
static h2_bk_uart_tunnel_decoder_t s_decoder;
static int s_started;
static volatile int s_running;
static uint8_t *s_uart_rx_storage;
static h2_bk_cp_byte_ring_t s_uart_rx_ring;
static uart_id_t s_uart_id = UART_ID_MAX;
static int s_uart_rx_taken;
static int s_ap_ready;

static void write_ap_probe(uint8_t stage) {
  char line[48];
  int length = snprintf(line, sizeof(line),
                        "H2_BK_AP_PROBE stage=%u\r\n", (unsigned)stage);
  if (length <= 0 || (size_t)length >= sizeof(line)) {
    return;
  }
  (void)bk_uart_set_enable_tx(s_uart_id, true);
  if (bk_uart_write_bytes(s_uart_id, line, (uint32_t)length) == BK_OK) {
    bk_uart_wait_tx_over(s_uart_id);
  }
}

static void uart_rx_isr(uart_id_t uart_id, void *param) {
  uint8_t buffer[H2_BK_CP_UART_CHUNK_SIZE];
  size_t count = 0u;
  uint8_t byte;
  (void)param;

  /* Own the physical RX ISR while the CP transport is active. Merely setting
   * the shell RX indication callback is insufficient when CONFIG_CLI is off:
   * the callback depends on the shell's private ISR and 200-byte ring having
   * already been installed. Drain the UART FIFO directly into the transport
   * ring so loader input does not depend on the CLI lifecycle. */
  while (uart_read_byte_ex(uart_id, &byte) != -1) {
    buffer[count++] = byte;
    if (count == sizeof(buffer)) {
      (void)h2_bk_cp_byte_ring_push(&s_uart_rx_ring, buffer, count);
      count = 0u;
    }
  }
  if (count != 0u) {
    (void)h2_bk_cp_byte_ring_push(&s_uart_rx_ring, buffer, count);
  }
}

static void uart_rx_poll(void) {
  /* The RX interrupt drains the FIFO while this task is blocked forwarding a
   * response to the host. Polling is still required for short frames that do
   * not reliably raise RX-finish on BK7258. Exclude the ISR while manually
   * invoking the same producer so the transport ring remains single-writer. */
  uint32_t interrupt_level = rtos_enter_critical();
  uart_rx_isr(s_uart_id, NULL);
  rtos_exit_critical(interrupt_level);
}

static uint16_t uart_rx_read(uint8_t *buffer, uint16_t capacity) {
  uint32_t interrupt_level = rtos_enter_critical();
  size_t count = h2_bk_cp_byte_ring_pop(&s_uart_rx_ring, buffer, capacity);
  rtos_exit_critical(interrupt_level);
  return (uint16_t)count;
}

static int uart_rx_take_overflow(void) {
  uint32_t interrupt_level = rtos_enter_critical();
  int overflow = h2_bk_cp_byte_ring_take_overflow(&s_uart_rx_ring);
  rtos_exit_critical(interrupt_level);
  return overflow;
}

static void uart_rx_release(void) {
  if (!s_uart_rx_taken) {
    return;
  }
  (void)bk_uart_disable_rx_interrupt(s_uart_id);
  (void)bk_uart_recover_rx_isr(s_uart_id);
  s_uart_rx_taken = 0;
  s_uart_id = UART_ID_MAX;
}

typedef struct mailbox_writer_context {
  uint8_t mailbox_id;
} mailbox_writer_context_t;

static uint16_t mailbox_write_chunk(void *user, const uint8_t *data,
                                    uint16_t len) {
  mailbox_writer_context_t *context = user;
  return bk_mb_uart_write(context->mailbox_id, (uint8_t *)data, len);
}

static void mailbox_write_wait(void *user) {
  (void)user;
  rtos_delay_milliseconds(1u);
}

static int mailbox_write_all(uint8_t mailbox_id, const uint8_t *data,
                             size_t len) {
  mailbox_writer_context_t context = {.mailbox_id = mailbox_id};
  return h2_bk_cp_write_all(&s_running, mailbox_write_chunk,
                            mailbox_write_wait, &context, data, len);
}

static int suspend_logs(void *user) {
  (void)user;
  if (shell_uart.dev_drv == NULL || shell_uart.dev_drv->io_ctrl == NULL) {
    return -1;
  }
  shell_log_flush();
  return shell_uart.dev_drv->io_ctrl(
             &shell_uart, SHELL_IO_CTRL_TX_SUSPEND, NULL)
             ? 0
             : -1;
}

static int write_uart_frame(void *user, const uint8_t *data, size_t len) {
  (void)user;
  if (len > UINT16_MAX) {
    return -1;
  }
  if (bk_uart_set_enable_tx(s_uart_id, true) != BK_OK) {
    return -1;
  }
  return bk_uart_write_bytes(s_uart_id, data, (uint32_t)len) == BK_OK ? 0
                                                                     : -1;
}

static void wait_uart_frame(void *user) {
  (void)user;
  shell_log_flush();
  uint8_t uart_id = 0u;
  if (shell_uart.dev_drv != NULL && shell_uart.dev_drv->io_ctrl != NULL &&
      shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_GET_UART_PORT,
                                  &uart_id)) {
    bk_uart_wait_tx_over((uart_id_t)uart_id);
  }
}

static void resume_logs(void *user) {
  (void)user;
  (void)shell_uart.dev_drv->io_ctrl(
      &shell_uart, SHELL_IO_CTRL_TX_RESUME, NULL);
}

static int acknowledge_frame(void *user, uint16_t sequence) {
  (void)user;
  const uint8_t ack[] = {
      H2_BK_CP_TX_ACK_MAGIC,
      H2_BK_CP_TX_ACK_VERSION,
      (uint8_t)(sequence & 0xffu),
      (uint8_t)(sequence >> 8u),
  };
  return mailbox_write_all(MB_UART1, ack, sizeof(ack));
}

static int write_physical_frame(void *user, uint16_t sequence,
                                const uint8_t *data, size_t len) {
  (void)user;
  if (data == NULL || len == 0u || len > UINT16_MAX ||
      shell_uart.dev_drv == NULL || shell_uart.dev_drv->io_ctrl == NULL) {
    return -1;
  }
  /* Pause the shell serializer after its current FIFO contents drain, write
   * the protocol frame directly, then resume queued logs. */
  const h2_bk_cp_frame_writer_t writer = {
      .suspend_logs = suspend_logs,
      .write_frame = write_uart_frame,
      .wait_frame = wait_uart_frame,
      .resume_logs = resume_logs,
      .acknowledge = acknowledge_frame,
  };
  return h2_bk_cp_write_serialized_frame(&s_running, &writer, sequence, data,
                                         len);
}

static void transport_task(void *arg) {
  uint8_t uart_buffer[H2_BK_CP_UART_CHUNK_SIZE];
  uint8_t mailbox_buffer[H2_BK_CP_MAILBOX_CHUNK_SIZE];
  uint16_t uart_pending_offset = 0u;
  uint16_t uart_pending_count = 0u;
  (void)arg;

  while (s_running) {
    /* RX-finish is not raised reliably for short frames on BK7258. Poll the
     * hardware FIFO so the 22-byte session-open frame cannot remain below the
     * interrupt threshold indefinitely. */
    uart_rx_poll();
    uint8_t ready_request = 0u;
    while (bk_mb_uart_read(MB_UART1, &ready_request,
                           sizeof(ready_request)) != 0u) {
      if (ready_request == H2_BK_CP_READY_REQUEST) {
        uint8_t ready_ack = H2_BK_CP_READY_ACK;
        if (!s_ap_ready) {
          /* AP startup can reconfigure shared UART state after CP startup.
           * Restore the CP-owned receiver only when the AP endpoint is ready. */
          (void)bk_uart_set_enable_rx(s_uart_id, true);
          (void)bk_uart_enable_rx_interrupt(s_uart_id);
          s_ap_ready = 1;
        }
        (void)mailbox_write_all(MB_UART1, &ready_ack, sizeof(ready_ack));
      } else if (ready_request >= 0xe1u && ready_request <= 0xefu) {
        write_ap_probe((uint8_t)(ready_request - 0xe0u));
      }
    }
    if (uart_rx_take_overflow()) {
      os_printf("H2_BK_CP_TRANSPORT_ERROR reason=rx_overflow\r\n");
    }
    if (s_ap_ready && uart_pending_count == 0u) {
      uart_pending_count =
          uart_rx_read(uart_buffer, H2_BK_CP_UART_CHUNK_SIZE);
      uart_pending_offset = 0u;
    }
    uint16_t uart_written = 0u;
    if (uart_pending_count != 0u) {
      /* Never wait for AP mailbox space here. The AP can stop consuming while
       * it programs flash; blocking would also stop this task from draining
       * the physical UART FIFO and permanently lose KCP frames. Keep the
       * unsent tail locally, continue polling UART into the transport ring,
       * and retry after the AP makes room. */
      uart_written = bk_mb_uart_write(
          MB_UART0, uart_buffer + uart_pending_offset, uart_pending_count);
      uart_pending_offset += uart_written;
      uart_pending_count -= uart_written;
    }

    uint16_t mailbox_count =
        bk_mb_uart_read(MB_UART0, mailbox_buffer, sizeof(mailbox_buffer));
    if (mailbox_count != 0u) {
      (void)h2_bk_uart_tunnel_decoder_input(
          &s_decoder, mailbox_buffer, mailbox_count, write_physical_frame,
          NULL);
    }
    if (uart_written == 0u && mailbox_count == 0u) {
      rtos_delay_milliseconds(1u);
    }
  }
  h2_bk_uart_tunnel_decoder_init(&s_decoder);
  (void)rtos_set_semaphore(&s_transport_done);
  rtos_delete_thread(NULL);
}

int h2_bk_h2loader_cp_transport_start(void) {
  if (s_started) {
    return -1;
  }
  if (bk_mb_uart_dev_init(MB_UART0) != 0) {
    return -2;
  }
  if (bk_mb_uart_dev_init(MB_UART1) != 0) {
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -3;
  }
  s_uart_rx_storage = psram_malloc(H2_BK_CP_UART_RX_QUEUE_SIZE);
  if (s_uart_rx_storage == NULL) {
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -4;
  }
  h2_bk_cp_byte_ring_init(&s_uart_rx_ring, s_uart_rx_storage,
                          H2_BK_CP_UART_RX_QUEUE_SIZE);
  s_ap_ready = 0;
  uint8_t uart_port = UART_ID_MAX;
  if (shell_uart.dev_drv == NULL || shell_uart.dev_drv->io_ctrl == NULL ||
      !shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_GET_UART_PORT,
                                   &uart_port) ||
      uart_port >= UART_ID_MAX) {
    psram_free(s_uart_rx_storage);
    s_uart_rx_storage = NULL;
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -5;
  }
  s_uart_id = (uart_id_t)uart_port;
  if (bk_uart_set_baud_rate(s_uart_id, H2_BK_CP_UART_BAUD_RATE) != BK_OK ||
      bk_uart_take_rx_isr(s_uart_id, uart_rx_isr, NULL) != BK_OK) {
    s_uart_id = UART_ID_MAX;
    psram_free(s_uart_rx_storage);
    s_uart_rx_storage = NULL;
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -6;
  }
  s_uart_rx_taken = 1;
  (void)bk_uart_enable_rx_interrupt(s_uart_id);
  h2_bk_uart_tunnel_decoder_init(&s_decoder);
  s_running = 1;
  if (rtos_init_semaphore(&s_transport_done, 1) != kNoErr) {
    s_running = 0;
    psram_free(s_uart_rx_storage);
    s_uart_rx_storage = NULL;
    uart_rx_release();
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -7;
  }
  int rc = rtos_create_psram_thread(
      &s_transport_thread, BEKEN_APPLICATION_PRIORITY, "h2-uart-tunnel",
      transport_task, H2_BK_CP_TASK_STACK_SIZE, NULL);
  if (rc == 0) {
    s_started = 1;
  } else {
    s_running = 0;
    (void)rtos_deinit_semaphore(&s_transport_done);
    psram_free(s_uart_rx_storage);
    s_uart_rx_storage = NULL;
    uart_rx_release();
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
  }
  return rc;
}

int h2_bk_h2loader_cp_transport_stop(void) {
  if (!s_started) {
    return 0;
  }
  s_running = 0;
  if (rtos_get_semaphore(&s_transport_done, H2_BK_CP_STOP_TIMEOUT_MS) !=
      kNoErr) {
    return -1;
  }
  s_transport_thread = NULL;
  (void)rtos_deinit_semaphore(&s_transport_done);
  uart_rx_release();
  (void)bk_mb_uart_dev_deinit(MB_UART1);
  (void)bk_mb_uart_dev_deinit(MB_UART0);
  psram_free(s_uart_rx_storage);
  s_uart_rx_storage = NULL;
  s_started = 0;
  return 0;
}
