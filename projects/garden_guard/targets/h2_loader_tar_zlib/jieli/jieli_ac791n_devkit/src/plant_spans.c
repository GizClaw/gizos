#include "plant_spans.h"

#include <string.h>

/* Each group is one cell and one plant kind in lane/column order. */
struct plant_span_group {
  uint32_t input_offset;
  uint32_t input_size;
  uint32_t output_offset;
  uint32_t output_size;
};

#include "plant_spans_groups.inc"
#include "plant_spans_index.inc"

extern const uint8_t h2_garden_plant_spans_archive[];

enum { DICTIONARY_BYTES = 128 * 1024, OUTPUT_PER_STEP = 4096, COLUMNS = 9 };
_Static_assert(PLANT_SPAN_BYTES == BANK_BYTES, "plant bank outputs must match");

void h2_garden_guard_jieli_release_plant_spans(
    h2_garden_guard_jieli_plant_spans_t* spans) {
  if (spans->decoder != NULL) xz_dec_end(spans->decoder);
  if (spans->output != NULL)
    h2_pal_mem_free(spans->runtime->mem, spans->output);
  spans->decoder = NULL;
  spans->output = NULL;
  spans->finished = true;
}

static void write_status(const h2_garden_guard_jieli_plant_spans_t* spans,
                         h2_pal_log_level_t level, const char* message) {
  (void)h2_pal_log_write(spans->runtime->log, level, "garden-guard", message);
}

void h2_garden_guard_jieli_prepare_plant_spans(void* user) {
  h2_garden_guard_jieli_plant_spans_t* spans = user;
  if (spans->finished) return;
  if (spans->decoder == NULL) {
    spans->output = h2_pal_mem_alloc(spans->runtime->mem, PLANT_SPAN_BYTES);
    if (spans->output != NULL) {
      xz_crc32_init();
      spans->decoder = xz_dec_init(XZ_PREALLOC, DICTIONARY_BYTES);
    }
    if (spans->decoder == NULL) {
      h2_garden_guard_jieli_release_plant_spans(spans);
      write_status(spans, H2_PAL_LOG_WARN, "plant span allocation unavailable");
    }
    return;
  }
  const struct plant_span_group* group = &plant_span_groups[spans->prepared];
  struct xz_buf* buffer = &spans->buffer;
  if (buffer->out == NULL) {
    xz_dec_reset(spans->decoder);
    *buffer = (struct xz_buf){
        .in = h2_garden_plant_spans_archive + group->input_offset,
        .in_size = group->input_size,
        .out = spans->output + group->output_offset};
  }
  /* Limit output work per call and let the run loop maintain timing. XZ
   * validates the stream CRC before the group becomes visible to playback. */
  const size_t remaining = group->output_size - buffer->out_pos;
  buffer->out_size =
      buffer->out_pos +
      (remaining < OUTPUT_PER_STEP ? remaining : OUTPUT_PER_STEP);
  const enum xz_ret result = xz_dec_run(spans->decoder, buffer);
  if (result == XZ_OK) return;
  if (result != XZ_STREAM_END || buffer->out_pos != group->output_size ||
      buffer->in_pos != group->input_size) {
    h2_garden_guard_jieli_release_plant_spans(spans);
    write_status(spans, H2_PAL_LOG_WARN, "plant span decoding failed");
    return;
  }

  buffer->out = NULL;
  if (++spans->prepared == PLANT_SPAN_GROUPS) {
    xz_dec_end(spans->decoder);
    spans->decoder = NULL;
    spans->finished = true;
    write_status(spans, H2_PAL_LOG_INFO,
                 "H2_GARDEN_GUARD_PLANT_SPANS_READY groups=90");
  }
}

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

const uint8_t* h2_garden_guard_jieli_find_plant_spans(
    void* user, const h2_garden_guard_plant_span_key_t* key) {
  const h2_garden_guard_jieli_plant_spans_t* spans = user;
  if (spans->output == NULL || key->lane >= 5u || key->column >= COLUMNS ||
      (key->kind != 1u && key->kind != 2u))
    return NULL;
  const unsigned cell = key->lane * COLUMNS + key->column;
  if (cell * 2u + key->kind - 1u >= spans->prepared ||
      bank_projection[cell][0] != float_bits(key->x) ||
      bank_projection[cell][1] != float_bits(key->y) ||
      bank_projection[cell][2] != float_bits(key->scale))
    return NULL;
  const bool sunflower = key->kind == 1u;
  const unsigned slot =
      2u * ((sunflower ? 0u : 20u) +
            (unsigned)(key->idle_tick % (sunflower ? 20u : 14u))) +
      (unsigned)key->blink;
  const uint32_t offset = bank_offsets[cell][slot];
  if (offset == 0u || bank_sway[cell][slot] != float_bits(key->sway))
    return NULL;
  return spans->output + offset - 1u;
}
