#include "h2_bk_cp_transport_core.h"

#include <limits.h>

void h2_bk_cp_byte_ring_init(h2_bk_cp_byte_ring_t *ring, uint8_t *data,
                             size_t capacity) {
  if (ring == NULL) {
    return;
  }
  ring->data = data;
  ring->capacity = capacity;
  ring->read_offset = 0u;
  ring->write_offset = 0u;
  ring->overflowed = 0;
}

size_t h2_bk_cp_byte_ring_push(h2_bk_cp_byte_ring_t *ring,
                               const uint8_t *data, size_t len) {
  if (ring == NULL || ring->data == NULL || ring->capacity < 2u ||
      (data == NULL && len != 0u)) {
    return 0u;
  }
  size_t pushed = 0u;
  while (pushed < len) {
    size_t next = (ring->write_offset + 1u) % ring->capacity;
    if (next == ring->read_offset) {
      ring->overflowed = 1;
      break;
    }
    ring->data[ring->write_offset] = data[pushed++];
    ring->write_offset = next;
  }
  return pushed;
}

size_t h2_bk_cp_byte_ring_pop(h2_bk_cp_byte_ring_t *ring, uint8_t *data,
                              size_t capacity) {
  if (ring == NULL || ring->data == NULL ||
      (data == NULL && capacity != 0u)) {
    return 0u;
  }
  size_t count = 0u;
  while (count < capacity && ring->read_offset != ring->write_offset) {
    data[count++] = ring->data[ring->read_offset];
    ring->read_offset = (ring->read_offset + 1u) % ring->capacity;
  }
  return count;
}

int h2_bk_cp_byte_ring_take_overflow(h2_bk_cp_byte_ring_t *ring) {
  if (ring == NULL) {
    return 0;
  }
  int overflowed = ring->overflowed;
  ring->overflowed = 0;
  if (overflowed) {
    ring->read_offset = ring->write_offset;
  }
  return overflowed;
}

int h2_bk_cp_write_all(volatile int *running, h2_bk_cp_write_fn write,
                       h2_bk_cp_wait_fn wait, void *user,
                       const uint8_t *data, size_t len) {
  if (running == NULL || write == NULL || wait == NULL ||
      (data == NULL && len != 0u)) {
    return -1;
  }
  while (len != 0u) {
    if (!*running) {
      return -1;
    }
    uint16_t take = len > UINT16_MAX ? UINT16_MAX : (uint16_t)len;
    uint16_t written = write(user, data, take);
    if (written > take) {
      return -1;
    }
    if (written == 0u) {
      wait(user);
      continue;
    }
    data += written;
    len -= written;
  }
  return 0;
}

int h2_bk_cp_write_serialized_frame(
    volatile int *running, const h2_bk_cp_frame_writer_t *writer,
    uint16_t sequence, const uint8_t *data, size_t len) {
  if (running == NULL || writer == NULL || writer->suspend_logs == NULL ||
      writer->write_frame == NULL || writer->wait_frame == NULL ||
      writer->resume_logs == NULL || writer->acknowledge == NULL ||
      data == NULL || len == 0u) {
    return -1;
  }
  if (!*running) {
    return -1;
  }
  if (writer->suspend_logs(writer->user) != 0) {
    return -1;
  }
  int rc = writer->write_frame(writer->user, data, len);
  writer->wait_frame(writer->user);
  writer->resume_logs(writer->user);
  if (rc != 0 || !*running) {
    return -1;
  }
  return writer->acknowledge(writer->user, sequence);
}
