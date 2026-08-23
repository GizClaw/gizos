#ifndef H2_BK_CP_TRANSPORT_CORE_H
#define H2_BK_CP_TRANSPORT_CORE_H

#include <stddef.h>
#include <stdint.h>

typedef struct h2_bk_cp_byte_ring {
  uint8_t *data;
  size_t capacity;
  volatile size_t read_offset;
  volatile size_t write_offset;
  volatile int overflowed;
} h2_bk_cp_byte_ring_t;

typedef uint16_t (*h2_bk_cp_write_fn)(void *user, const uint8_t *data,
                                      uint16_t len);
typedef void (*h2_bk_cp_wait_fn)(void *user);

typedef struct h2_bk_cp_frame_writer {
  void *user;
  int (*suspend_logs)(void *user);
  int (*write_frame)(void *user, const uint8_t *data, size_t len);
  void (*wait_frame)(void *user);
  void (*resume_logs)(void *user);
  int (*acknowledge)(void *user, uint16_t sequence);
} h2_bk_cp_frame_writer_t;

void h2_bk_cp_byte_ring_init(h2_bk_cp_byte_ring_t *ring, uint8_t *data,
                             size_t capacity);
size_t h2_bk_cp_byte_ring_push(h2_bk_cp_byte_ring_t *ring,
                               const uint8_t *data, size_t len);
size_t h2_bk_cp_byte_ring_pop(h2_bk_cp_byte_ring_t *ring, uint8_t *data,
                              size_t capacity);
int h2_bk_cp_byte_ring_take_overflow(h2_bk_cp_byte_ring_t *ring);

int h2_bk_cp_write_all(volatile int *running, h2_bk_cp_write_fn write,
                       h2_bk_cp_wait_fn wait, void *user,
                       const uint8_t *data, size_t len);
int h2_bk_cp_write_serialized_frame(
    volatile int *running, const h2_bk_cp_frame_writer_t *writer,
    uint16_t sequence, const uint8_t *data, size_t len);

#endif
