#ifndef H2_BM8563_FAKE_H
#define H2_BM8563_FAKE_H

#include "h2_bm8563_transport.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BM8563_FAKE_REGISTER_COUNT 16u
#define H2_BM8563_FAKE_OPERATION_COUNT_MAX 32u

typedef enum h2_bm8563_fake_operation_kind {
    H2_BM8563_FAKE_OPERATION_READ = 0,
    H2_BM8563_FAKE_OPERATION_WRITE,
} h2_bm8563_fake_operation_kind_t;

typedef struct h2_bm8563_fake_operation {
    h2_bm8563_fake_operation_kind_t kind;
    uint8_t start_register;
    size_t data_length;
} h2_bm8563_fake_operation_t;

typedef struct h2_bm8563_fake {
    uint8_t registers[H2_BM8563_FAKE_REGISTER_COUNT];
    h2_bm8563_fake_operation_t operations[H2_BM8563_FAKE_OPERATION_COUNT_MAX];
    size_t operation_count;
    size_t fail_operation;
    h2_pal_result_t fail_result;
} h2_bm8563_fake_t;

void h2_bm8563_fake_init(h2_bm8563_fake_t *fake);

h2_bm8563_transport_t h2_bm8563_fake_transport(h2_bm8563_fake_t *fake);

/** Fail one-based operation index, or zero to disable failure. */
void h2_bm8563_fake_fail_operation(h2_bm8563_fake_t *fake,
                                   size_t operation,
                                   h2_pal_result_t result);

#ifdef __cplusplus
}
#endif

#endif
