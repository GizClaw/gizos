#ifndef H2_ALIPAY_TAP_INTERNAL_H
#define H2_ALIPAY_TAP_INTERNAL_H

#include "h2_alipay_tap.h"
#include "h2_crypto.h"

typedef struct h2_alipay_tap_tag_details {
  h2_alipay_tap_tag_t tag;
  uint8_t amount[16];
  uint8_t reply_mode;
} h2_alipay_tap_tag_details_t;

int h2_alipay_tap_decode_details(const h2_alipay_client_config_t* config,
                                 const uint8_t* data, size_t length,
                                 h2_alipay_tap_tag_details_t* out);
int h2_alipay_tap_squeeze(const char* token, char output[16]);
int h2_alipay_tap_generate(h2_alipay_tap_t* tap, const char* token,
                           bool reset_write_result,
                           h2_alipay_client_payment_t* diagnostics);
int h2_alipay_tap_nfc_reply(h2_alipay_tap_t* tap, bool vip,
                            const uint8_t watch[2], char output[128],
                            size_t* length);
int h2_alipay_tap_ble_reply(h2_alipay_tap_t* tap,
                            h2_crypto_md5_aes_keyset_t* keys, bool vip,
                            uint8_t mode, const uint8_t watch[2],
                            uint8_t output[20]);
int h2_alipay_tap_cabinet_reply(h2_alipay_tap_t* tap, bool wallet,
                                char output[128], size_t* length);
int h2_alipay_tap_verify_device(const h2_pal_crypto_api_t* crypto,
                                const char* public_key, const char* signature);
int h2_alipay_tap_connected_reply(h2_alipay_tap_t* tap, const char* public_key,
                                  const char* payment_token, bool vip,
                                  uint8_t output[256], size_t* length,
                                  h2_alipay_client_payment_t* diagnostics);

#endif
