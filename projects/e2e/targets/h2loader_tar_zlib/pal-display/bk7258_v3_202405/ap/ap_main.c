#include "bk_private/bk_init.h"
#include "components/media_types.h"
#include "device_runner.h"
#include "frame_buffer.h"
#include "h2_bk7258_board.h"
#include "h2_bk_h2loader.h"
#include "h2_bk_platform_core.h"
#include "h2_bk_target_task_policy.h"
#include <os/os.h>
#include <stdio.h>
#include <string.h>
static h2_runtime_t *runtime;
static volatile int finished;
static uint16_t *scanout;
/* SDK reserves large display slabs separately from its 640KB AP PSRAM heap.
 * This launcher-owned Memory API uses that real pool for bitmap working sets;
 * it is unrelated to the Runtime Core allocator and PAL Display vtable. */
typedef struct bitmap_block {
  frame_buffer_t *frame;
  size_t length;
} bitmap_block_t;
#define BITMAP_HEADER_BYTES 16u
static void *bitmap_alloc(void *user, size_t length) {
  (void)user;
  if (length > UINT32_MAX - BITMAP_HEADER_BYTES)
    return NULL;
  frame_buffer_t *frame =
      frame_buffer_display_malloc((uint32_t)length + BITMAP_HEADER_BYTES);
  if (!frame)
    return NULL;
  bitmap_block_t header = {.frame = frame, .length = length};
  memcpy(frame->frame, &header, sizeof(header));
  return frame->frame + BITMAP_HEADER_BYTES;
}
static void bitmap_free(void *user, void *pointer) {
  (void)user;
  if (!pointer)
    return;
  bitmap_block_t header;
  memcpy(&header, (uint8_t *)pointer - BITMAP_HEADER_BYTES, sizeof(header));
  frame_buffer_display_free(header.frame);
}
static void *bitmap_realloc(void *user, void *pointer, size_t length) {
  if (!pointer)
    return bitmap_alloc(user, length);
  if (!length) {
    bitmap_free(user, pointer);
    return NULL;
  }
  bitmap_block_t header;
  memcpy(&header, (uint8_t *)pointer - BITMAP_HEADER_BYTES, sizeof(header));
  void *next = bitmap_alloc(user, length);
  if (!next)
    return NULL;
  memcpy(next, pointer, length < header.length ? length : header.length);
  bitmap_free(user, pointer);
  return next;
}
static const h2_pal_mem_vtable_t bitmap_vtable = {
    .alloc = bitmap_alloc, .realloc = bitmap_realloc, .free = bitmap_free};
static const h2_pal_mem_api_t bitmap_memory = {.vtable = &bitmap_vtable};
static int observe(void *user, const uint16_t *pixels, int width, int height,
                   uint32_t brightness, const char *id) {
  (void)user;
  if (width != 800 || height != 480)
    return H2_DISPLAY_ERR_IO;
  int rc = h2_bk7258_board_display_capture(scanout, (size_t)width * height);
  if (!rc && memcmp(pixels, scanout, (size_t)width * height * 2u))
    rc = H2_DISPLAY_ERR_IO;
  printf("H2_DISPLAY_OBSERVATION case=%s source=LCD-active-DMA "
         "refresh=observed optical_verified=0 brightness_command=%u rc=%d\n",
         id, (unsigned)brightness, rc);
  return rc;
}
static void hold(void) {
  for (;;)
    rtos_delay_milliseconds(1000);
}
static void fail(const char *stage, int rc) {
  printf("H2_DISPLAY_LAUNCHER_FAIL stage=%s rc=%d\n", stage, rc);
  hold();
}
static void watchdog(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(600000);
  if (!finished)
    puts("H2_DISPLAY_WATCHDOG timeout=600s");
  hold();
}
static void run(void *unused) {
  (void)unused;
  rtos_delay_milliseconds(5000);
  /* Prime the real process-owned media slab without leaving Display open.
   * The shared App still starts from closed state and checks every operation.
   */
  int prepare = h2_pal_display_open(runtime->display);
  if (!prepare)
    prepare = h2_pal_display_close(runtime->display);
  if (prepare)
    fail("media-prepare", prepare);
  scanout = h2_pal_mem_alloc(&bitmap_memory, 800u * 480u * 2u);
  if (!scanout)
    fail("capture-allocation", H2_DISPLAY_ERR_NO_MEMORY);
  int rc = h2_display_device_run(runtime, H2_DISPLAY_VERSION, observe, NULL,
                                 &bitmap_memory);
  h2_pal_mem_free(&bitmap_memory, scanout);
  scanout = NULL;
  finished = 1;
  if (rc != H2_PAL_OK) {
    printf("H2_DISPLAY_QUALIFICATION_FAIL rc=%d confirm=not-attempted\n", rc);
    fail("qualification", rc);
  }
  int confirm = h2_bk_h2loader_confirm_current_app(runtime);
  if (confirm != H2_PAL_OK)
    fail("confirm", confirm);
  printf("H2_DISPLAY_READY rc=%d confirm=%d\n", rc, confirm);
  int visual = h2_display_device_demo(runtime);
  if (visual)
    fail("visual-demo", visual);
  visual = h2_display_device_show_pattern(runtime);
  if (visual)
    fail("stable-pattern", visual);
  printf("H2_DISPLAY_STABLE brightness=100 rc=%d optical_verified=0\n", visual);
  for (;;) {
    h2_display_device_replay(runtime);
    rtos_delay_milliseconds(3000);
  }
}
static void entry(void *unused) {
  (void)unused;
  h2_runtime_config_t config;
  int rc = h2_bk7258_board_runtime_config(&config);
  if (rc)
    fail("board", rc);
  config.system_event = h2_pal_unsupported_system_event_api();
  rc = h2_runtime_init(&config, &runtime);
  if (rc)
    fail("runtime", rc);
  rc = h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
      runtime, "pal-display", H2_LOADER_CAPABILITY_UART);
  if (rc)
    fail("commands", rc);
  beken_thread_t timer;
  if (rtos_core0_create_psram_thread(&timer, 7, "store_deadline", watchdog,
                                     4096, NULL) != kNoErr)
    fail("watchdog", -12);
  const h2_pal_task_options_t options = {.name = "pal-display/e2e/runner",
                                         .min_stack_size = 65536};
  h2_pal_task_t *task = NULL;
  rc = h2_pal_task_start(runtime->task, &options, run, NULL, &task);
  if (rc)
    fail("runner", rc);
  hold();
}
int main(void) {
  int rc = h2_bk_target_task_policy_install();
  if (rc)
    return -1;
  bk_init();
  rc = h2_bk7258_board_start_entry_task("bk/pal-display", entry, NULL);
  if (rc)
    fail("entry", rc);
  return 0;
}
