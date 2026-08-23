#include "fake_trng.h"

#include "h2_bk3633_platform_entropy.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    fake_trng_t fake;
    uint8_t output[9];

    fake_trng_init(&fake);
    fake_trng_install(&fake);
    assert(h2_bk3633_platform_entropy_fill(NULL, NULL, 0u) == H2_PAL_OK);
    assert(h2_bk3633_platform_entropy_fill(NULL, NULL, 1u) ==
           H2_PAL_ERR_INVALID_ARG);
    assert(h2_bk3633_platform_entropy_fill(NULL, output, sizeof(output)) ==
           H2_PAL_OK);
    assert(output[0] == 0x40u && output[3] == 0x10u);
    assert(output[4] == 0x41u && output[8] == 0x42u);
    assert(fake.enable_count == 1u && fake.next_word == 3u);

    fake_trng_init(&fake);
    fake.would_block_reads = 1024u;
    fake_trng_install(&fake);
    memset(output, 0xa5, sizeof(output));
    assert(h2_bk3633_platform_entropy_fill(NULL, output, 4u) ==
           H2_PAL_ERR_TIMEOUT);
    assert(output[0] == 0u && output[3] == 0u);

    fake_trng_init(&fake);
    fake.words[1] = fake.words[0];
    fake_trng_install(&fake);
    memset(output, 0xa5, sizeof(output));
    assert(h2_bk3633_platform_entropy_fill(NULL, output, 8u) ==
           H2_PAL_ERR_IO);
    assert(output[0] == 0u && output[7] == 0u);

    fake_trng_init(&fake);
    fake.fail_after_words = 1u;
    fake.failure = H2_PAL_ERR_IO;
    fake_trng_install(&fake);
    memset(output, 0xa5, sizeof(output));
    assert(h2_bk3633_platform_entropy_fill(NULL, output, 8u) ==
           H2_PAL_ERR_IO);
    assert(output[0] == 0u && output[7] == 0u);

    fake_trng_uninstall();
    memset(output, 0xa5, sizeof(output));
    assert(h2_bk3633_platform_entropy_fill(NULL, output, 1u) ==
           H2_PAL_ERR_UNAVAILABLE);
    assert(output[0] == 0u);
    return 0;
}
