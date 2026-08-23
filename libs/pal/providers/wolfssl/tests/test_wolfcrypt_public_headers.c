#include "h2_wolfcrypt_crypto.h"
#include "h2_wolfssl.h"

int main(void) {
    h2_wolfcrypt_crypto_config_t config = {0};
    return config.entropy == 0 ? 0 : 1;
}
