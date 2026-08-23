#ifndef PEER_H_
#define PEER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "peer_connection.h"
int peer_init(
    const h2_pal_mem_api_t* mem,
    const h2_pal_crypto_api_t* crypto);

void peer_deinit();

#ifdef __cplusplus
}
#endif

#endif  // PEER_H_
