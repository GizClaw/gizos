#ifndef H2_ONENET_INTERNAL_H
#define H2_ONENET_INTERNAL_H

#include "h2_onenet.h"

typedef struct h2_onenet_writer {
  char* data;
  size_t capacity;
  size_t length;
  h2_pal_result_t result;
} h2_onenet_writer_t;

void h2_onenet_write(h2_onenet_writer_t* writer, const char* data, size_t len);
void h2_onenet_text(h2_onenet_writer_t* writer, const char* text);
void h2_onenet_decimal(h2_onenet_writer_t* writer, uint64_t value);
bool h2_onenet_segment_valid(const char* name);
void h2_onenet_wipe(void* data, size_t len);

#endif
