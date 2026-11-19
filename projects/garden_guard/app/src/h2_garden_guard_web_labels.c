#ifdef __EMSCRIPTEN__
#include "h2_garden_guard_render.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

/* clang-format off */
EM_JS(void, apply_menu_labels, (const h2_garden_guard_menu_labels_t* menu,
                                unsigned labels_offset, unsigned label_stride,
                                unsigned text_offset), {
  const present = Module['h2GardenMenuLabels'];
  if (!present) return;
  if (!menu) { present(null); return; }
  const base = menu >> 2;
  const count = HEAPU32[base];
  const foreground = HEAPU32[base + 1];
  const card = Array.from(HEAPF32.subarray(base + 2, base + 6));
  const shade = HEAPF32[base + 6];
  const labels = [];
  for (let i = 0; i < count; ++i) {
    const address = menu + labels_offset + i * label_stride;
    const n = address >> 2;
    const [x, y, width, height] = HEAPF32.subarray(n, n + 4);
    const rgb = HEAPU32[n + 4];
    const color = `rgb(${(rgb >> 11) * 255 / 31} ${((rgb >> 5) & 63) * 255 / 63} ${(rgb & 31) * 255 / 31})`;
    labels.push({x, y, width, height, color, alpha: HEAPU32[n + 5] / 256,
      centered: !!HEAPU32[n + 6], text: UTF8ToString(address + text_offset, 80)});
  }
  present({labels, foreground, card, shade});
});
/* clang-format on */

_Static_assert(offsetof(h2_garden_guard_menu_labels_t, shade) == 24u,
               "Web label metadata layout changed");
_Static_assert(offsetof(h2_garden_guard_menu_label_t, centered) == 24u,
               "Web label layout changed");

static void present_on_main(void* data) {
  const h2_garden_guard_menu_labels_t* const* menu = data;
  apply_menu_labels(*menu, offsetof(h2_garden_guard_menu_labels_t, labels),
                    sizeof(h2_garden_guard_menu_label_t),
                    offsetof(h2_garden_guard_menu_label_t, text));
}

void h2_garden_guard_web_present_menu_labels(
    const h2_garden_guard_menu_labels_t* menu) {
  (void)emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(),
                              present_on_main, &menu);
}
#else
/* Keep a nonempty translation unit under pedantic native toolchains. */
typedef int h2_garden_guard_web_labels_unavailable_t;
#endif
