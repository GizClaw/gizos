#include "h2_bk7258_board.h"

#include "components/media_types.h"
#include "frame_buffer.h"
#include <string.h>

/* Display bitmap working sets use the SDK media slabs, independently of the
 * smaller AP control heap. The slab owner lives for the image lifetime. */
typedef struct bitmap_block {
  frame_buffer_t* frame;
  size_t length;
} bitmap_block_t;
#define BITMAP_HEADER_BYTES 16u
_Static_assert(sizeof(bitmap_block_t) <= BITMAP_HEADER_BYTES,
               "bitmap allocation header must fit before the pixels");

static void* bitmap_alloc(void* user, size_t length) {
  (void)user;
  if (length > UINT32_MAX - BITMAP_HEADER_BYTES) return NULL;
  frame_buffer_t* frame =
      frame_buffer_display_malloc((uint32_t)length + BITMAP_HEADER_BYTES);
  if (!frame) return NULL;
  bitmap_block_t header = {.frame = frame, .length = length};
  memcpy(frame->frame, &header, sizeof(header));
  return frame->frame + BITMAP_HEADER_BYTES;
}
static void bitmap_free(void* user, void* pointer) {
  (void)user;
  if (!pointer) return;
  bitmap_block_t header;
  memcpy(&header, (uint8_t*)pointer - BITMAP_HEADER_BYTES, sizeof(header));
  frame_buffer_display_free(header.frame);
}
static void* bitmap_realloc(void* user, void* pointer, size_t length) {
  if (!pointer) return bitmap_alloc(user, length);
  if (!length) {
    bitmap_free(user, pointer);
    return NULL;
  }
  bitmap_block_t header;
  memcpy(&header, (uint8_t*)pointer - BITMAP_HEADER_BYTES, sizeof(header));
  void* next = bitmap_alloc(user, length);
  if (!next) return NULL;
  memcpy(next, pointer, length < header.length ? length : header.length);
  bitmap_free(user, pointer);
  return next;
}

const h2_pal_mem_api_t* h2_bk7258_board_display_allocator(void) {
  static const h2_pal_mem_vtable_t methods = {
      .alloc = bitmap_alloc, .realloc = bitmap_realloc, .free = bitmap_free};
  static const h2_pal_mem_api_t memory = {.vtable = &methods};
  return &memory;
}
