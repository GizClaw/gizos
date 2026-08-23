#include "h2_bk_cp_transport_core.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef struct write_capture {
  volatile int *running;
  uint8_t output[32];
  size_t output_len;
  uint16_t max_write;
  int blocked_writes;
  int waits;
  int stop_on_wait;
} write_capture_t;

typedef struct frame_capture {
  volatile int *running;
  uint8_t output[32];
  size_t output_len;
  uint8_t pending_log[8];
  size_t pending_log_len;
  char events[8];
  size_t event_count;
  uint16_t acknowledged_sequence;
  int suspended;
  int stop_while_waiting;
} frame_capture_t;

static uint16_t capture_write(void *user, const uint8_t *data, uint16_t len) {
  write_capture_t *capture = user;
  if (capture->blocked_writes > 0) {
    capture->blocked_writes -= 1;
    return 0u;
  }
  uint16_t take = len < capture->max_write ? len : capture->max_write;
  assert(capture->output_len + take <= sizeof(capture->output));
  memcpy(capture->output + capture->output_len, data, take);
  capture->output_len += take;
  return take;
}

static void capture_wait(void *user) {
  write_capture_t *capture = user;
  capture->waits += 1;
  if (capture->stop_on_wait) {
    *capture->running = 0;
  }
}

static void frame_event(frame_capture_t *capture, char event) {
  assert(capture->event_count < sizeof(capture->events));
  capture->events[capture->event_count++] = event;
}

static void frame_log(frame_capture_t *capture, uint8_t value) {
  if (capture->suspended) {
    assert(capture->pending_log_len < sizeof(capture->pending_log));
    capture->pending_log[capture->pending_log_len++] = value;
    return;
  }
  assert(capture->output_len < sizeof(capture->output));
  capture->output[capture->output_len++] = value;
}

static int frame_suspend(void *user) {
  frame_capture_t *capture = user;
  frame_event(capture, 'S');
  capture->suspended = 1;
  return 0;
}

static int frame_write(void *user, const uint8_t *data, size_t len) {
  frame_capture_t *capture = user;
  frame_event(capture, 'W');
  assert(capture->suspended);
  assert(capture->output_len + len <= sizeof(capture->output));
  memcpy(capture->output + capture->output_len, data, len);
  capture->output_len += len;
  frame_log(capture, 'l');
  return 0;
}

static void frame_wait(void *user) {
  frame_capture_t *capture = user;
  frame_event(capture, 'T');
  if (capture->stop_while_waiting) {
    *capture->running = 0;
  }
}

static void frame_resume(void *user) {
  frame_capture_t *capture = user;
  frame_event(capture, 'R');
  capture->suspended = 0;
  assert(capture->output_len + capture->pending_log_len <=
         sizeof(capture->output));
  memcpy(capture->output + capture->output_len, capture->pending_log,
         capture->pending_log_len);
  capture->output_len += capture->pending_log_len;
  capture->pending_log_len = 0u;
}

static int frame_acknowledge(void *user, uint16_t sequence) {
  frame_capture_t *capture = user;
  frame_event(capture, 'A');
  capture->acknowledged_sequence = sequence;
  return 0;
}

static h2_bk_cp_frame_writer_t frame_writer(frame_capture_t *capture) {
  const h2_bk_cp_frame_writer_t writer = {
      .user = capture,
      .suspend_logs = frame_suspend,
      .write_frame = frame_write,
      .wait_frame = frame_wait,
      .resume_logs = frame_resume,
      .acknowledge = frame_acknowledge,
  };
  return writer;
}

static void test_binary_ring_partial_read_and_wrap(void) {
  uint8_t storage[6];
  h2_bk_cp_byte_ring_t ring;
  const uint8_t first[] = {0u, 1u, 2u, 3u};
  const uint8_t second[] = {4u, 5u, 6u};
  uint8_t output[8] = {0};

  h2_bk_cp_byte_ring_init(&ring, storage, sizeof(storage));
  assert(h2_bk_cp_byte_ring_push(&ring, first, sizeof(first)) == sizeof(first));
  assert(h2_bk_cp_byte_ring_pop(&ring, output, 2u) == 2u);
  assert(memcmp(output, first, 2u) == 0);
  assert(h2_bk_cp_byte_ring_push(&ring, second, sizeof(second)) ==
         sizeof(second));
  assert(h2_bk_cp_byte_ring_pop(&ring, output, sizeof(output)) == 5u);
  const uint8_t expected[] = {2u, 3u, 4u, 5u, 6u};
  assert(memcmp(output, expected, sizeof(expected)) == 0);
}

static void test_queue_full_fails_closed(void) {
  uint8_t storage[5];
  h2_bk_cp_byte_ring_t ring;
  const uint8_t input[] = {0u, 1u, 2u, 3u, 4u};
  uint8_t output[5];

  h2_bk_cp_byte_ring_init(&ring, storage, sizeof(storage));
  assert(h2_bk_cp_byte_ring_push(&ring, input, sizeof(input)) == 4u);
  assert(h2_bk_cp_byte_ring_take_overflow(&ring) == 1);
  assert(h2_bk_cp_byte_ring_pop(&ring, output, sizeof(output)) == 0u);
  assert(h2_bk_cp_byte_ring_take_overflow(&ring) == 0);
  assert(h2_bk_cp_byte_ring_push(&ring, input, 2u) == 2u);
  assert(h2_bk_cp_byte_ring_pop(&ring, output, sizeof(output)) == 2u);
  assert(memcmp(output, input, 2u) == 0);
}

static void test_mailbox_partial_write_backpressure(void) {
  volatile int running = 1;
  const uint8_t input[] = {0u, 1u, 2u, 3u, 4u};
  write_capture_t capture = {
      .running = &running,
      .max_write = 2u,
      .blocked_writes = 1,
  };

  assert(h2_bk_cp_write_all(&running, capture_write, capture_wait, &capture,
                            input, sizeof(input)) == 0);
  assert(capture.waits == 1);
  assert(capture.output_len == sizeof(input));
  assert(memcmp(capture.output, input, sizeof(input)) == 0);
}

static void test_mailbox_stop_while_blocked(void) {
  volatile int running = 1;
  const uint8_t input[] = {1u};
  write_capture_t capture = {
      .running = &running,
      .max_write = 1u,
      .blocked_writes = 1,
      .stop_on_wait = 1,
  };

  assert(h2_bk_cp_write_all(&running, capture_write, capture_wait, &capture,
                            input, sizeof(input)) == -1);
  assert(capture.waits == 1);
  assert(capture.output_len == 0u);
}

static void test_log_cannot_interleave_complete_frame(void) {
  volatile int running = 1;
  const uint8_t frame[] = {'F', 'R', 'A', 'M', 'E'};
  frame_capture_t capture = {.running = &running};
  h2_bk_cp_frame_writer_t writer = frame_writer(&capture);

  frame_log(&capture, 'L');
  assert(h2_bk_cp_write_serialized_frame(&running, &writer, 0x1234u, frame,
                                         sizeof(frame)) == 0);
  const uint8_t expected[] = {'L', 'F', 'R', 'A', 'M', 'E', 'l'};
  assert(capture.output_len == sizeof(expected));
  assert(memcmp(capture.output, expected, sizeof(expected)) == 0);
  assert(capture.event_count == 5u);
  assert(memcmp(capture.events, "SWTRA", 5u) == 0);
  assert(capture.acknowledged_sequence == 0x1234u);
}

static void test_frame_stop_while_waiting_resumes_logs_without_ack(void) {
  volatile int running = 1;
  const uint8_t frame[] = {'F'};
  frame_capture_t capture = {
      .running = &running,
      .stop_while_waiting = 1,
  };
  h2_bk_cp_frame_writer_t writer = frame_writer(&capture);

  assert(h2_bk_cp_write_serialized_frame(&running, &writer, 7u, frame,
                                         sizeof(frame)) == -1);
  assert(capture.suspended == 0);
  assert(capture.event_count == 4u);
  assert(memcmp(capture.events, "SWTR", 4u) == 0);
  assert(capture.acknowledged_sequence == 0u);
}

int main(void) {
  test_binary_ring_partial_read_and_wrap();
  test_queue_full_fails_closed();
  test_mailbox_partial_write_backpressure();
  test_mailbox_stop_while_blocked();
  test_log_cannot_interleave_complete_frame();
  test_frame_stop_while_waiting_resumes_logs_without_ack();
  return 0;
}
