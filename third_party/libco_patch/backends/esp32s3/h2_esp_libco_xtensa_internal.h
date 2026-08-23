#ifndef H2_ESP_LIBCO_XTENSA_INTERNAL_H
#define H2_ESP_LIBCO_XTENSA_INTERNAL_H

#define H2_ESP_LIBCO_CTX_SP 0
#define H2_ESP_LIBCO_CTX_A0 4
#define H2_ESP_LIBCO_CTX_PS 8
#define H2_ESP_LIBCO_CTX_SAR 12
#define H2_ESP_LIBCO_CTX_LBEG 16
#define H2_ESP_LIBCO_CTX_LEND 20
#define H2_ESP_LIBCO_CTX_LCOUNT 24
#define H2_ESP_LIBCO_CTX_STACK_TOP 28
#define H2_ESP_LIBCO_CTX_ENTRYPOINT 32
#define H2_ESP_LIBCO_CTX_STARTED 36
#define H2_ESP_LIBCO_CTX_SIZE 40
#define H2_ESP_LIBCO_STACK_ALIGNMENT 16

#ifndef __ASSEMBLER__

#include <stdint.h>

typedef struct h2_esp_libco_xtensa_context {
  uint32_t sp;
  uint32_t a0;
  uint32_t ps;
  uint32_t sar;
  uint32_t lbeg;
  uint32_t lend;
  uint32_t lcount;
  uint32_t stack_top;
  uint32_t entrypoint;
  uint32_t started;
} h2_esp_libco_xtensa_context_t;

void h2_esp_libco_xtensa_swap(h2_esp_libco_xtensa_context_t *next,
                              h2_esp_libco_xtensa_context_t *previous);

#endif

#endif
