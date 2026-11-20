#ifdef __EMSCRIPTEN__
#include "h2_garden_guard_render.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

/* clang-format off */
EM_JS(void, apply_labels, (const h2_garden_guard_labels_t* parts,
                           const h2_garden_guard_labels_t* scene, unsigned field,
                           bool paused, bool with_artwork, unsigned hud_alpha,
                           unsigned set_size, unsigned entries_offset,
                           unsigned entry_size, unsigned text_offset), {
  const present = Module['h2GardenLabels'];
  if (!present) return;
  function readLabels(address) {
    const count = HEAPU32[address >> 2];
    const labels = [];
    for (let i = 0; i < count; ++i) {
      const entry = address + entries_offset + i * entry_size;
      const word = entry >> 2;
      const [x, y, width, height] = HEAPF32.subarray(word, word + 4);
      const rgb = HEAPU32[word + 4];
      const color = `rgb(${(rgb >> 11) * 255 / 31} ${((rgb >> 5) & 63) * 255 / 63} ${(rgb & 31) * 255 / 31})`;
      labels.push({x, y, width, height, color, alpha: HEAPU32[word + 5] / 256,
        centered: !!HEAPU32[word + 6], text: UTF8ToString(entry + text_offset, 80)});
    }
    return labels;
  }
  function readSuns(address) {
    const word = address >> 2;
    const suns = [];
    for (let i = 0; i < HEAPU32[word + 6]; ++i) {
      const start = word + 7 + i * 4;
      suns.push(Array.from(HEAPF32.subarray(start, start + 4)));
    }
    return suns;
  }
  const hud = [];
  for (let i = 0; i < field; ++i) hud.push(...readLabels(parts + i * set_size));
  const panel = parts + field * set_size;
  const word = scene >> 2;
  present({hud, panel: readLabels(panel), scene: readLabels(scene),
    hudAlpha: hud_alpha / 256,
    card: Array.from(HEAPF32.subarray(word + 1, word + 5)),
    shade: HEAPF32[word + 5], suns: readSuns(paused ? panel : scene)}, with_artwork);
});
/* clang-format on */

_Static_assert(offsetof(h2_garden_guard_labels_t, suns) == 28u,
               "Web label set layout changed");
_Static_assert(offsetof(h2_garden_guard_label_t, centered) == 24u,
               "Web label layout changed");

typedef struct web_labels_frame {
  const h2_garden_guard_labels_t* parts;
  const h2_garden_guard_labels_t* scene;
  unsigned field;
  bool paused, with_artwork;
} web_labels_frame_t;

static void present_on_main(void* data) {
  const web_labels_frame_t* frame = data;
  apply_labels(frame->parts, frame->scene, frame->field, frame->paused,
               frame->with_artwork,
               frame->paused ? 256u - H2_GARDEN_GUARD_PAUSE_SHADE : 256u,
               sizeof(h2_garden_guard_labels_t),
               offsetof(h2_garden_guard_labels_t, labels),
               sizeof(h2_garden_guard_label_t),
               offsetof(h2_garden_guard_label_t, text));
}

void h2_garden_guard_web_present_labels(const h2_garden_guard_labels_t* parts,
                                        const h2_garden_guard_labels_t* scene,
                                        unsigned field, bool paused,
                                        bool with_artwork) {
  web_labels_frame_t frame = {parts, scene, field, paused, with_artwork};
  (void)emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(),
                              present_on_main, &frame);
}
#else
/* Keep a nonempty translation unit under pedantic native toolchains. */
typedef int h2_garden_guard_web_labels_unavailable_t;
#endif
