#include "esp_loader_io.h"
#include "slip.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static const uint8_t* input;
static size_t input_size;
static size_t input_offset;

esp_loader_error_t loader_port_read(uint8_t* data, uint16_t size,
                                    uint32_t timeout) {
  (void)timeout;
  if (size > input_size - input_offset) return ESP_LOADER_ERROR_TIMEOUT;
  memcpy(data, input + input_offset, size);
  input_offset += size;
  return ESP_LOADER_SUCCESS;
}
esp_loader_error_t loader_port_write(const uint8_t* data, uint16_t size,
                                     uint32_t timeout) {
  (void)data;
  (void)size;
  (void)timeout;
  return ESP_LOADER_SUCCESS;
}
uint32_t loader_port_remaining_time(void) { return 1000; }

static void packet(const uint8_t* encoded, size_t encoded_size,
                   const uint8_t* expected, size_t expected_size) {
  uint8_t decoded[32] = {0};
  size_t received = 0;
  input = encoded;
  input_size = encoded_size;
  input_offset = 0;
  assert(SLIP_receive_packet(decoded, sizeof(decoded), &received) == 0);
  assert(received == expected_size);
  assert(memcmp(decoded, expected, received) == 0);
}

int main(void) {
  const uint8_t escaped_delimiter[] = {0xc0, 0xdb, 0xdc, 0x10, 0xc0};
  const uint8_t delimiter_payload[] = {0xc0, 0x10};
  packet(escaped_delimiter, sizeof(escaped_delimiter), delimiter_payload,
         sizeof(delimiter_payload));
  const uint8_t escaped_escape[] = {0xc0, 0xdb, 0xdd, 0x20, 0xc0};
  const uint8_t escape_payload[] = {0xdb, 0x20};
  packet(escaped_escape, sizeof(escaped_escape), escape_payload,
         sizeof(escape_payload));
  const uint8_t normal[] = {0xc0, 0xc0, 0x30, 0xdb, 0xdc, 0xdb, 0xdd, 0xc0};
  const uint8_t normal_payload[] = {0x30, 0xc0, 0xdb};
  packet(normal, sizeof(normal), normal_payload, sizeof(normal_payload));
  return 0;
}
