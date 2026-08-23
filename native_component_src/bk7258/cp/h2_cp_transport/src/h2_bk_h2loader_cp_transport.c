#include "h2_bk_h2loader_cp_transport.h"
#include "h2_bk_cp_transport_core.h"
#include "h2_bk_uart_tunnel_codec.h"

#include <stdbool.h>

#include "common/bk_include.h"
#include "driver/mb_uart_driver.h"
#include "driver/uart.h"
#include "os/os.h"
#include "shell_drv.h"

#include <stddef.h>
#include <stdint.h>

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

static beken_thread_t s_transport_thread;
static beken_semaphore_t s_transport_done;
static h2_bk_uart_tunnel_decoder_t s_decoder;
static int s_started;
static volatile int s_running;
static uint8_t s_uart_rx_storage[H2_BK_CP_UART_RX_QUEUE_SIZE];
static h2_bk_cp_byte_ring_t s_uart_rx_ring;

static void uart_rx_indicate(void) {
  uint8_t buffer[H2_BK_CP_UART_CHUNK_SIZE];
  uint16_t count;

  /* Drain the SDK's 200-byte shell ring from the UART ISR into a queue sized
   * beyond the complete KCP receive window. The transport task may block on
   * AP mailbox backpressure without silently losing the next UART burst. */
  while ((count = shell_uart.dev_drv->read(&shell_uart, buffer,
                                           sizeof(buffer))) != 0u) {
    (void)h2_bk_cp_byte_ring_push(&s_uart_rx_ring, buffer, count);
  }
}

static uint16_t uart_rx_read(uint8_t *buffer, uint16_t capacity) {
  uint32_t interrupt_level = rtos_enter_critical();
  size_t count = h2_bk_cp_byte_ring_pop(&s_uart_rx_ring, buffer, capacity);
  rtos_exit_critical(interrupt_level);
  return (uint16_t)count;
}

static int uart_rx_take_overflow(void) {
  uint16_t shell_overflow = 0u;
  (void)shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_GET_RX_STATUS,
                                    &shell_overflow);
  uint32_t interrupt_level = rtos_enter_critical();
  int overflow = h2_bk_cp_byte_ring_take_overflow(&s_uart_rx_ring);
  if (shell_overflow != 0u) {
    s_uart_rx_ring.read_offset = s_uart_rx_ring.write_offset;
    overflow = 1;
  }
  rtos_exit_critical(interrupt_level);
  return overflow;
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
  return shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_TX_SUSPEND,
                                     NULL)
             ? 0
             : -1;
}

static int write_uart_frame(void *user, const uint8_t *data, size_t len) {
  (void)user;
  int rc = bk_uart_set_enable_tx(UART_ID_0, true);
  return rc == 0 ? bk_uart_write_bytes(UART_ID_0, data, (uint32_t)len) : rc;
}

static void wait_uart_frame(void *user) {
  (void)user;
  bk_uart_wait_tx_over(UART_ID_0);
}

static void resume_logs(void *user) {
  (void)user;
  (void)shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_TX_RESUME, NULL);
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
  /* Pause the shell serializer after its current FIFO contents drain. Any
   * remaining or newly queued log bytes stay pending until resume, so this
   * complete protocol frame cannot be interleaved with console output. */
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
  uint8_t buffer[H2_BK_CP_MAILBOX_CHUNK_SIZE];
  (void)arg;

  while (s_running) {
    if (uart_rx_take_overflow()) {
      os_printf("H2_BK_CP_TRANSPORT_ERROR reason=rx_overflow\r\n");
    }
    uint16_t uart_count = uart_rx_read(buffer, H2_BK_CP_UART_CHUNK_SIZE);
    if (uart_count != 0u) {
      if (mailbox_write_all(MB_UART0, buffer, uart_count) != 0) {
        break;
      }
    }

    uint16_t mailbox_count = bk_mb_uart_read(MB_UART0, buffer, sizeof(buffer));
    if (mailbox_count != 0u) {
      (void)h2_bk_uart_tunnel_decoder_input(&s_decoder, buffer, mailbox_count,
                                            write_physical_frame, NULL);
    }
    if (uart_count == 0u && mailbox_count == 0u) {
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
    return -1;
  }
  if (bk_mb_uart_dev_init(MB_UART1) != 0) {
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -1;
  }
  h2_bk_cp_byte_ring_init(&s_uart_rx_ring, s_uart_rx_storage,
                          sizeof(s_uart_rx_storage));
  if (bk_uart_set_baud_rate(UART_ID_0, H2_BK_CP_UART_BAUD_RATE) != 0 ||
      shell_uart.dev_drv == NULL || shell_uart.dev_drv->read == NULL ||
      shell_uart.dev_drv->io_ctrl == NULL ||
      !shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_SET_RX_ISR,
                                   (void *)uart_rx_indicate)) {
    if (shell_uart.dev_drv != NULL && shell_uart.dev_drv->io_ctrl != NULL) {
      (void)shell_uart.dev_drv->io_ctrl(
          &shell_uart, SHELL_IO_CTRL_SET_RX_ISR, NULL);
    }
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -1;
  }
  h2_bk_uart_tunnel_decoder_init(&s_decoder);
  s_running = 1;
  if (rtos_init_semaphore(&s_transport_done, 1) != kNoErr) {
    s_running = 0;
    (void)shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_SET_RX_ISR,
                                      NULL);
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
    return -1;
  }
  int rc = rtos_create_thread(&s_transport_thread, BEKEN_APPLICATION_PRIORITY,
                              "h2-uart-tunnel", transport_task,
                              H2_BK_CP_TASK_STACK_SIZE, NULL);
  if (rc == 0) {
    s_started = 1;
  } else {
    s_running = 0;
    (void)rtos_deinit_semaphore(&s_transport_done);
    (void)shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_SET_RX_ISR,
                                      NULL);
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
  (void)shell_uart.dev_drv->io_ctrl(&shell_uart, SHELL_IO_CTRL_SET_RX_ISR,
                                    NULL);
  (void)bk_mb_uart_dev_deinit(MB_UART1);
  (void)bk_mb_uart_dev_deinit(MB_UART0);
  s_started = 0;
  return 0;
}
