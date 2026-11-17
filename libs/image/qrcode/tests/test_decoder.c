#include "h2_qrcode_decoder.h"

#include "h2_qrcode.h"
#include "h2_app_test_mem.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint8_t image[320u * 320u], saved[sizeof(image)];
static uint8_t modules[H2_QRCODE_BUFFER_LEN_FOR_VERSION(10)];
static uint8_t scratch[sizeof(modules)];

static size_t render(const char* text, h2_qrcode_ecc_t ecc, unsigned rotation,
                     bool mirror, size_t* stride) {
  h2_qrcode_t code;
  assert(h2_qrcode_encode_text(text, ecc, 10, modules, sizeof(modules), scratch,
                               sizeof(scratch), &code) == 0);
  size_t width = ((size_t)code.size + 8u) * 4u;
  *stride = width + 3u;
  assert(width * *stride <= sizeof(image));
  memset(image, 0x7a, sizeof(image));
  for (size_t row = 0u; row < width; ++row) {
    for (size_t column = 0u; column < width; ++column) {
      int horizontal = (int)(column / 4u) - 4;
      int vertical = (int)(row / 4u) - 4;
      if (mirror) horizontal = code.size - horizontal - 1;
      for (unsigned turn = 0u; turn < rotation; ++turn) {
        int previous = horizontal;
        horizontal = vertical;
        vertical = code.size - previous - 1;
      }
      image[row * *stride + column] =
          h2_qrcode_module_is_dark(&code, horizontal, vertical) ? 0u : 255u;
    }
  }
  memcpy(saved, image, sizeof(image));
  return width;
}

static void expect_unchanged(const uint8_t* bytes, size_t length) {
  for (size_t index = 0u; index < length; ++index)
    assert(bytes[index] == 0xa5u);
}

int main(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  static const char* texts[] = {"https://qr.alipay.com/TEST_SYNTHETIC_CODE",
                                "SYNTHETIC TRANSIT TEXT",
                                "012345678901234567890"};
  unsigned scenarios = 0u;
  uint8_t output[1026];
  for (size_t index = 0u; index < sizeof(texts) / sizeof(*texts); ++index) {
    for (unsigned level = 0u; level < 4u; ++level) {
      for (unsigned direction = 0u; direction < 8u; ++direction) {
        size_t stride = 0u;
        size_t width = render(texts[index], (h2_qrcode_ecc_t)level,
                              direction % 4u, direction >= 4u, &stride);
        memset(output, 0xa5, sizeof(output));
        size_t length = 999u;
        assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width,
                                     width, stride, output + 1u,
                                     sizeof(output) - 2u, &length) == 0);
        assert(length == strlen(texts[index]) &&
               memcmp(output + 1u, texts[index], length) == 0);
        assert(output[0] == 0xa5u && output[length + 1u] == 0xa5u);
        assert(memcmp(image, saved, sizeof(image)) == 0);
        assert(memory.live_blocks == 0u);
        ++scenarios;
      }
    }
  }
  size_t stride = 0u;
  size_t width = render(texts[0], H2_QRCODE_ECC_MEDIUM, 0u, false, &stride);
  size_t length = 0u, required = strlen(texts[0]);
  memset(output, 0xa5, sizeof(output));
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width, width,
                               stride, output, required - 1u,
                               &length) == H2_PAL_ERR_NO_SPACE);
  assert(length == required);
  expect_unchanged(output, sizeof(output));
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width, width,
                               stride, NULL, 0u,
                               &length) == H2_PAL_ERR_NO_SPACE);
  assert(length == required && memory.live_blocks == 0u);
  size_t start = memory.calls;
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width, width,
                               stride, output, sizeof(output), &length) == 0);
  size_t allocations = memory.calls - start;
  for (size_t fail = 1u; fail <= allocations; ++fail) {
    memory.fail_at = memory.calls + fail;
    memset(output, 0xa5, sizeof(output));
    assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width,
                                 width, stride, output, sizeof(output),
                                 &length) == H2_PAL_ERR_NO_MEMORY);
    assert(length == 0u && memory.live_blocks == 0u);
    expect_unchanged(output, sizeof(output));
    memory.fail_at = 0u;
  }
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width, width,
                               width - 1u, output, sizeof(output),
                               &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, SIZE_MAX,
                               width, stride, output, sizeof(output),
                               &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_decode_luma(&memory.api, image, 1u, width, width, stride,
                               output, sizeof(output),
                               &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width, width,
                               stride, image + 1u, 10u,
                               &length) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_qrcode_decode_luma(NULL, image, width * stride, width, width,
                               stride, output, sizeof(output),
                               &length) == H2_PAL_ERR_INVALID_ARG);
  memset(image, 255, sizeof(image));
  memset(output, 0xa5, sizeof(output));
  assert(h2_qrcode_decode_luma(&memory.api, image, width * stride, width, width,
                               stride, output, sizeof(output),
                               &length) == H2_PAL_ERR_NOT_FOUND);
  assert(length == 0u && memory.live_blocks == 0u);
  expect_unchanged(output, sizeof(output));
  printf("QR decode scenarios=%u allocation failure points=%zu\n", scenarios,
         allocations);
  return 0;
}
