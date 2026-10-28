#include <assert.h>
#include <stdint.h>
#include "h2_runtime.h"
#define H2_MOSAICO_LAUNCHER_HOST_TEST 1
#define H2_MOSAICO_SUITE_CAMERA 1
static int64_t esp_timer_get_time(void) { return 0; }
#include "projects/e2e/targets/h2loader_tar_zlib/mosaico/esp_mosaico/tests/launcher_under_test.h"

static uint8_t pixels[1280u * 720u * 2u];
static struct {
  int starts, acquires, releases, stops, borrowed;
  int start_error, acquire_error, release_errors, stop_errors, malformed;
} fake;
static int start(void* user) {
  (void)user;
  ++fake.starts;
  return fake.start_error;
}
static int acquire(void* user, h2_pal_camera_frame_t* frame) {
  (void)user;
  ++fake.acquires;
  if (fake.acquire_error) return fake.acquire_error;
  assert(!fake.borrowed);
  fake.borrowed = 1;
  *frame = (h2_pal_camera_frame_t){.data = pixels,
                                   .size = sizeof(pixels),
                                   .width = fake.malformed ? 1260 : 1280,
                                   .height = 720,
                                   .stride = 2560,
                                   .format = H2_PAL_CAMERA_FORMAT_UYVY,
                                   .token = 42};
  return 0;
}
static int release(void* user, const h2_pal_camera_frame_t* frame) {
  (void)user;
  ++fake.releases;
  assert(fake.borrowed && frame->data == pixels && frame->token == 42);
  if (fake.release_errors) {
    --fake.release_errors;
    return H2_PAL_ERR_IO;
  }
  fake.borrowed = 0;
  return 0;
}
static int stop(void* user) {
  (void)user;
  ++fake.stops;
  if (fake.borrowed) return H2_PAL_ERR_BUSY;
  if (fake.stop_errors) {
    --fake.stop_errors;
    return H2_PAL_ERR_IO;
  }
  return 0;
}
static const h2_pal_camera_vtable_t ops = {
    .start = start, .acquire = acquire, .release = release, .stop = stop};
static const h2_pal_camera_api_t api = {.vtable = &ops};
static h2_runtime_t fixture;
static void prepare(void) {
  assert(!fake.borrowed);
  memset(&fake, 0, sizeof(fake));
  memset(&loan, 0, sizeof(loan));
  fixture.camera = &api;
  runtime = &fixture;
}
int main(void) {
  prepare();
  assert(camera_run() == 0);
  assert(fake.starts == 10 && fake.acquires == 300 && fake.releases == 300 &&
         fake.stops == 310);
  prepare();
  fake.release_errors = 2;
  assert(camera_run() == 0 && fake.releases == 302 && !fake.borrowed);
  prepare();
  fake.start_error = H2_PAL_ERR_UNAVAILABLE;
  fake.stop_errors = 1;
  assert(camera_run() == H2_PAL_ERR_UNAVAILABLE && fake.stops == 2 &&
         !fake.acquires);
  prepare();
  fake.acquire_error = H2_PAL_ERR_TIMEOUT;
  assert(camera_run() == H2_PAL_ERR_TIMEOUT && fake.stops == 1 &&
         !fake.releases);
  prepare();
  fake.malformed = 1;
  assert(camera_run() == H2_PAL_ERR_IO && fake.releases == 1 && !fake.borrowed);
  prepare();
  fake.release_errors = 3;
  assert(camera_run() == H2_PAL_ERR_IO && fake.releases == 3 &&
         fake.stops == 1);
  assert(fake.borrowed && loan.data == pixels && loan.token == 42);
  assert(h2_pal_camera_release(&api, &loan) == 0);
  assert(h2_pal_camera_stop(&api) == 0);
  prepare();
  fake.stop_errors = 3;
  assert(camera_run() == H2_PAL_ERR_IO && fake.stops == 33 && !fake.borrowed);
  return 0;
}
