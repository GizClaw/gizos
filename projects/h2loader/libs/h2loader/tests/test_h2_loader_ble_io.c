#include "h2_loader_ble.h"
#include "h2_loader_app_client.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
  uint8_t data[8192];
  size_t capacity, size;
  unsigned writes, flushes;
  int write_error, flush_error, read_error;
  size_t read_size;
} fixture_t;

int h2_bleikcp_write(h2_bleikcp_t* stream, const uint8_t* data, size_t len,
                     uint32_t timeout_ms) {
  fixture_t* f = (fixture_t*)stream;
  (void)timeout_ms;
  assert(f != NULL);
  ++f->writes;
  if (len > f->capacity) return H2_PAL_ERR_NO_SPACE;
  if (f->write_error) return f->write_error;
  assert(len <= sizeof(f->data) - f->size);
  if (len != 0) memcpy(f->data + f->size, data, len);
  f->size += len;
  return 0;
}
int h2_bleikcp_read(h2_bleikcp_t* stream, uint8_t* data, size_t capacity,
                    size_t* out_size, uint32_t timeout_ms) {
  fixture_t* f = (fixture_t*)stream;
  (void)timeout_ms;
  *out_size = 0;
  if (f->read_error) return f->read_error;
  assert(f->read_size <= capacity);
  if (f->read_size != 0) memcpy(data, f->data, f->read_size);
  *out_size = f->read_size;
  return 0;
}
int h2_bleikcp_flush(h2_bleikcp_t* stream, uint32_t timeout_ms) {
  fixture_t* f = (fixture_t*)stream;
  (void)timeout_ms;
  ++f->flushes;
  return f->flush_error;
}

static void transfer(size_t capacity) {
  uint8_t message[8192];
  for (size_t i = 0; i < sizeof(message); ++i)
    message[i] = (uint8_t)(i * 17u + i / 256u);
  fixture_t f = {.capacity = capacity};
  h2_command_io_api_t io = h2_loader_ble_command_io((h2_bleikcp_t*)&f);
  size_t sent = 0;
  while (sent < sizeof(message)) {
    size_t written = 0;
    assert(io.vtable->write(io.user, message + sent, sizeof(message) - sent,
                            &written, 1000) == 0);
    assert(written != 0 && written <= capacity);
    sent += written;
  }
  assert(f.size == sizeof(message) && memcmp(f.data, message, f.size) == 0);
  assert(io.vtable->flush(io.user) == 0 && f.flushes == 1);

  f.size = f.writes = f.flushes = 0;
  assert(h2_loader_ble_app_write(&f, (const char*)message, sizeof(message)) ==
         0);
  assert(f.size == sizeof(message) && memcmp(f.data, message, f.size) == 0);
  assert(f.flushes == 1);
  assert(h2_loader_ble_app_write(&f, NULL, 0) == 0 && f.flushes == 2);
}

int main(void) {
  transfer(512);
  transfer(513);
  transfer(2048);
  transfer(8192);
  fixture_t f = {.capacity = 2048, .write_error = H2_PAL_ERR_TIMEOUT};
  h2_command_io_api_t io = h2_loader_ble_command_io((h2_bleikcp_t*)&f);
  uint8_t message[4096] = {0};
  size_t written = 123;
  assert(io.vtable->write(io.user, message, sizeof(message), &written, 1) ==
         H2_PAL_ERR_TIMEOUT);
  assert(written == 0 && f.size == 0);
  assert(h2_loader_ble_app_write(&f, (const char*)message, sizeof(message)) ==
         H2_PAL_ERR_TIMEOUT);
  assert(f.flushes == 0);
  assert(io.vtable->write(io.user, NULL, 1, &written, 1) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(io.vtable->write(io.user, message, 1, NULL, 1) ==
         H2_PAL_ERR_INVALID_ARG);
  f.write_error = 0;
  f.flush_error = H2_PAL_ERR_IO;
  assert(h2_loader_ble_app_write(&f, NULL, 0) == H2_PAL_ERR_IO);
  f.data[0] = 0x51;
  f.read_size = 1;
  assert(h2_loader_ble_app_read_byte(&f, 1) == 0x51);
  f.read_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_loader_ble_app_read_byte(&f, 1) == EOF);
  f.read_error = H2_PAL_ERR_CLOSED;
  assert(h2_loader_ble_app_read_byte(&f, 1) ==
         H2_LOADER_APP_CLIENT_SESSION_CLOSED);
  return 0;
}
