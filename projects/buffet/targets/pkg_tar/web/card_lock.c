#include "card_lock.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

/* Acquiring a Web Lock schedules a browser task. Polling lets the game keep
 * playing the closing cadence and drawing the receipt while another tab owns
 * the save. The worker does every Pref operation through the usual Web PAL. */
/* clang-format off */
EM_JS(int, try_card_lock, (void), {
  const locks = globalThis.navigator?.locks;
  if (!locks) return -3;
  let request = Module['buffetCardLock'];
  if (!request) {
    request = {release: null, timer: 0};
    Module['buffetCardLock'] = request;
    locks.request('gizos:buffet_cards_v1', {ifAvailable: true}, lock => {
      if (!lock) { delete Module['buffetCardLock']; return; }
      return new Promise(release => {
        request.release = release;
        // A tab suspended between the grant and its next frame yields the lock.
        request.timer = setTimeout(() => { delete Module['buffetCardLock']; release(); }, 750);
      });
    }).catch(() => { request.error = -2; });
    return -9;
  }
  if (request.error) { delete Module['buffetCardLock']; return request.error; }
  if (!request.release) return -9;
  clearTimeout(request.timer);
  return 0;
});
EM_JS(void, release_card_lock, (void), {
  const request = Module['buffetCardLock'];
  delete Module['buffetCardLock'];
  request.release();
});
/* clang-format on */

static void try_on_main(void* result) { *(int*)result = try_card_lock(); }
static void release_on_main(void* unused) {
  (void)unused;
  release_card_lock();
}

h2_pal_result_t buffet_web_with_card_lock(void* user,
                                          h2_buffet_card_work_fn work,
                                          void* context) {
  (void)user;
  em_proxying_queue* queue = emscripten_proxy_get_system_queue();
  pthread_t main_thread = emscripten_main_runtime_thread_id();
  int result = H2_PAL_ERR_UNAVAILABLE;
  if (!emscripten_proxy_sync(queue, main_thread, try_on_main, &result) ||
      result != H2_PAL_OK)
    return result;
  result = work(context);
  return emscripten_proxy_sync(queue, main_thread, release_on_main, NULL)
             ? result
             : H2_PAL_ERR_UNAVAILABLE;
}
