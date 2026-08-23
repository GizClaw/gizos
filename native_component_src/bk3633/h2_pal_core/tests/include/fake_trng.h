#ifndef FAKE_TRNG_H
#define FAKE_TRNG_H

#include "h2/pal/core/h2_pal_errors.h"

#include <stddef.h>
#include <stdint.h>

#define FAKE_TRNG_WORD_COUNT 16u

typedef struct fake_trng {
    uint32_t words[FAKE_TRNG_WORD_COUNT];
    size_t word_count;
    size_t next_word;
    uint32_t would_block_reads;
    size_t fail_after_words;
    h2_pal_result_t failure;
    size_t enable_count;
} fake_trng_t;

void fake_trng_init(fake_trng_t *fake);
void fake_trng_install(fake_trng_t *fake);
void fake_trng_uninstall(void);

#endif
