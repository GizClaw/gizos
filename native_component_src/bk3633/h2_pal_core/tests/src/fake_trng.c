#include "fake_trng.h"

#include <string.h>

typedef void (*h2_bk3633_trng_enable_fn)(void *user);
typedef h2_pal_result_t (*h2_bk3633_trng_read_fn)(
    void *user,
    uint32_t *out_word);

void h2_bk3633_entropy_set_driver_for_test(
    h2_bk3633_trng_enable_fn enable,
    h2_bk3633_trng_read_fn read,
    void *user);
void h2_bk3633_entropy_reset_driver_for_test(void);

static void fake_enable(void *user)
{
    fake_trng_t *fake = (fake_trng_t *)user;
    ++fake->enable_count;
}

static h2_pal_result_t fake_read(void *user, uint32_t *out_word)
{
    fake_trng_t *fake = (fake_trng_t *)user;
    if (fake->would_block_reads != 0u) {
        --fake->would_block_reads;
        return H2_PAL_ERR_WOULD_BLOCK;
    }
    if (fake->failure != H2_PAL_OK &&
        fake->next_word >= fake->fail_after_words) {
        return fake->failure;
    }
    if (fake->next_word >= fake->word_count) {
        return H2_PAL_ERR_IO;
    }
    *out_word = fake->words[fake->next_word++];
    return H2_PAL_OK;
}

void fake_trng_init(fake_trng_t *fake)
{
    size_t index;
    memset(fake, 0, sizeof(*fake));
    fake->word_count = FAKE_TRNG_WORD_COUNT;
    fake->fail_after_words = SIZE_MAX;
    for (index = 0u; index < fake->word_count; ++index) {
        fake->words[index] = 0x10203040u + (uint32_t)(index * 0x01010101u);
    }
}

void fake_trng_install(fake_trng_t *fake)
{
    h2_bk3633_entropy_set_driver_for_test(fake_enable, fake_read, fake);
}

void fake_trng_uninstall(void)
{
    h2_bk3633_entropy_reset_driver_for_test();
}
