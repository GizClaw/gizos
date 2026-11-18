#ifndef H2_FRUIT_SLICE_TEXT_H
#define H2_FRUIT_SLICE_TEXT_H

#include "h2_fruit_slice.h"

#include <stddef.h>
#include <stdint.h>

typedef struct h2_fruit_slice_translation {
  const char* en;
  const char* zh;
} h2_fruit_slice_translation_t;

extern const h2_fruit_slice_translation_t h2_fruit_slice_translations[];
extern const size_t h2_fruit_slice_translation_count;

/* Unknown strings (including numeric popups) pass through unchanged. */
const char* h2_fruit_slice_translate(h2_fruit_slice_language_t language,
                                     const char* english);
/* Strict UTF-8, advancing at least one byte on malformed input. */
uint32_t h2_fruit_slice_utf8_next(const char** text);

#endif
