#include "h2_qrcode_decoder.h"

#include "quirc.h"
#include <limits.h>
#include <string.h>

typedef struct decoder_workspace {
  struct quirc_code code;
  struct quirc_data data;
} decoder_workspace_t;

static void* decoder_alloc(void* user, size_t size) {
  return h2_pal_mem_alloc(user, size);
}

static void decoder_free(void* user, void* pointer) {
  h2_pal_mem_free(user, pointer);
}

static void decoder_wipe(void* data, size_t length) {
  volatile uint8_t* bytes = data;
  while (length-- != 0u) *bytes++ = 0u;
}

h2_pal_result_t h2_qrcode_decode_luma(const h2_pal_mem_api_t* memory,
                                      const uint8_t* image, size_t image_size,
                                      size_t width, size_t height,
                                      size_t stride, uint8_t* output,
                                      size_t capacity, size_t* length) {
  if (length == NULL) return H2_PAL_ERR_INVALID_ARG;
  *length = 0u;
  if (memory == NULL || memory->vtable == NULL ||
      memory->vtable->alloc == NULL || memory->vtable->free == NULL ||
      image == NULL || width == 0u || height == 0u || width > INT_MAX ||
      height > INT_MAX || stride < width || width > SIZE_MAX / height ||
      (output == NULL && capacity != 0u))
    return H2_PAL_ERR_INVALID_ARG;
  if (height > 1u && stride > (SIZE_MAX - width) / (height - 1u))
    return H2_PAL_ERR_INVALID_ARG;
  size_t required = (height - 1u) * stride + width;
  if (required > image_size) return H2_PAL_ERR_INVALID_ARG;
  if (output != NULL && capacity != 0u) {
    uintptr_t first = (uintptr_t)image, second = (uintptr_t)output;
    if ((second >= first && second - first < image_size) ||
        (first >= second && first - second < capacity))
      return H2_PAL_ERR_INVALID_ARG;
  }
  decoder_workspace_t* work = h2_pal_mem_alloc(memory, sizeof(*work));
  if (work == NULL) return H2_PAL_ERR_NO_MEMORY;
  memset(work, 0, sizeof(*work));
  const struct quirc_allocator allocator = {(void*)memory, decoder_alloc,
                                            decoder_free};
  struct quirc* recognizer = quirc_new_with_allocator(&allocator);
  uint8_t* pixels = NULL;
  int result = H2_PAL_ERR_NO_MEMORY;
  if (recognizer == NULL ||
      quirc_resize(recognizer, (int)width, (int)height) != 0)
    goto done;
  pixels = quirc_begin(recognizer, NULL, NULL);
  if (pixels == NULL) {
    result = H2_PAL_ERR_INVALID_STATE;
    goto done;
  }
  for (size_t row = 0u; row < height; ++row)
    memcpy(pixels + row * width, image + row * stride, width);
  quirc_end(recognizer);
  int count = quirc_count(recognizer);
  if (count != 1) {
    result = count == 0 ? H2_PAL_ERR_NOT_FOUND : H2_PAL_ERR_FORMAT;
    goto done;
  }
  quirc_extract(recognizer, 0, &work->code);
  quirc_decode_error_t decoded = quirc_decode(&work->code, &work->data);
  if (decoded != QUIRC_SUCCESS) {
    quirc_flip(&work->code);
    decoded = quirc_decode(&work->code, &work->data);
  }
  if (decoded != QUIRC_SUCCESS || work->data.payload_len < 0 ||
      work->data.payload_len > QUIRC_MAX_PAYLOAD) {
    result = H2_PAL_ERR_FORMAT;
    goto done;
  }
  *length = (size_t)work->data.payload_len;
  if (*length > capacity) {
    result = H2_PAL_ERR_NO_SPACE;
    goto done;
  }
  if (*length != 0u) memcpy(output, work->data.payload, *length);
  result = H2_PAL_OK;
done:
  if (pixels != NULL) decoder_wipe(pixels, width * height);
  if (recognizer != NULL) quirc_destroy(recognizer);
  decoder_wipe(work, sizeof(*work));
  h2_pal_mem_free(memory, work);
  return result;
}
