#include "h2_wolfcrypt_crypto.h"
#include "h2_wolfssl.h"

int main() {
    h2_wolfcrypt_crypto_config_t config{};
    return config.entropy == nullptr ? 0 : 1;
}
