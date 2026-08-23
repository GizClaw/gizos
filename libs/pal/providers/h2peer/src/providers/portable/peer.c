#include "peer.h"
#include "config.h"
#include "h2_libsrtp.h"

int peer_init(const h2_pal_mem_api_t *mem, const h2_pal_crypto_api_t *crypto) {
  if (mem == NULL || crypto == NULL) {
    return -1;
  }
  const h2_libsrtp_config_t config = {
      .mem = *mem,
      .crypto = *crypto,
      .max_packet_size = CONFIG_MTU,
  };
  if (h2_libsrtp_init(&config) != H2_PAL_OK) {
    return -1;
  }
  return 0;
}

void peer_deinit() {
  (void)h2_libsrtp_deinit();
}
