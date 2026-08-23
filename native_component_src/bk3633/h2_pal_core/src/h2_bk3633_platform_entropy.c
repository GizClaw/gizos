#include "h2_bk3633_platform_entropy.h"

#include "h2/pal/core/h2_pal_errors.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(BK3633)
#include "BK3633_RegList.h"
#endif

#define H2_BK3633_TRNG_POLL_LIMIT 1024u

typedef void (*h2_bk3633_trng_enable_fn)(void *user);
typedef h2_pal_result_t (*h2_bk3633_trng_read_fn)(
    void *user,
    uint32_t *out_word);

typedef struct h2_bk3633_entropy_state {
    h2_bk3633_trng_enable_fn enable;
    h2_bk3633_trng_read_fn read;
    void *user;
    uint32_t previous_word;
    bool has_previous_word;
} h2_bk3633_entropy_state_t;

static h2_bk3633_entropy_state_t s_entropy;

#if defined(BK3633)
static void sdk_trng_enable(void *user)
{
    (void)user;
    setf_TRNG_Reg0x0_trng_en;
}

static h2_pal_result_t sdk_trng_read(void *user, uint32_t *out_word)
{
    (void)user;
    *out_word = (uint32_t)addTRNG_Reg0x1;
    return H2_PAL_OK;
}
#endif

#if !defined(BK3633)
void h2_bk3633_entropy_set_driver_for_test(
    h2_bk3633_trng_enable_fn enable,
    h2_bk3633_trng_read_fn read,
    void *user)
{
    s_entropy = (h2_bk3633_entropy_state_t){
        .enable = enable,
        .read = read,
        .user = user,
    };
}

void h2_bk3633_entropy_reset_driver_for_test(void)
{
    memset(&s_entropy, 0, sizeof(s_entropy));
}
#endif

static void entropy_ensure_driver(void)
{
#if defined(BK3633)
    if (s_entropy.enable == NULL && s_entropy.read == NULL) {
        s_entropy.enable = sdk_trng_enable;
        s_entropy.read = sdk_trng_read;
    }
#endif
}

int h2_bk3633_platform_entropy_fill(void *user, uint8_t *out, size_t len)
{
    size_t written = 0u;
    (void)user;
    if (out == NULL && len != 0u) {
        return H2_PAL_ERR_INVALID_ARG;
    }
    if (len == 0u) {
        return H2_PAL_OK;
    }
    entropy_ensure_driver();
    if (s_entropy.enable == NULL || s_entropy.read == NULL) {
        memset(out, 0, len);
        return H2_PAL_ERR_UNAVAILABLE;
    }
    s_entropy.enable(s_entropy.user);
    while (written < len) {
        uint32_t word = 0u;
        uint32_t poll;
        h2_pal_result_t rc = H2_PAL_ERR_TIMEOUT;
        for (poll = 0u; poll < H2_BK3633_TRNG_POLL_LIMIT; ++poll) {
            rc = s_entropy.read(s_entropy.user, &word);
            if (rc != H2_PAL_ERR_WOULD_BLOCK) {
                break;
            }
        }
        if (rc == H2_PAL_ERR_WOULD_BLOCK) {
            rc = H2_PAL_ERR_TIMEOUT;
        }
        if (rc != H2_PAL_OK ||
            (s_entropy.has_previous_word && word == s_entropy.previous_word)) {
            memset(out, 0, len);
            return rc == H2_PAL_OK ? H2_PAL_ERR_IO : rc;
        }
        s_entropy.previous_word = word;
        s_entropy.has_previous_word = true;
        for (size_t byte = 0u; byte < sizeof(word) && written < len; ++byte) {
            out[written++] = (uint8_t)(word >> (byte * 8u));
        }
    }
    return H2_PAL_OK;
}
