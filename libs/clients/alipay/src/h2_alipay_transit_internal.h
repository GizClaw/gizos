#ifndef H2_ALIPAY_TRANSIT_INTERNAL_H
#define H2_ALIPAY_TRANSIT_INTERNAL_H

#include "h2_alipay_client.h"

typedef struct h2_alipay_transit_card {
  char number[40], type[40], title[40];
} h2_alipay_transit_card_t;

typedef struct h2_alipay_transit {
  int card_result, code_result;
  bool registered;
  h2_alipay_transit_card_t last;
} h2_alipay_transit_t;

int h2_alipay_transit_activity(const h2_alipay_client_config_t* config,
                               bool online, bool* active);
int h2_alipay_transit_cards(const h2_alipay_client_config_t* config,
                            h2_alipay_transit_t* state, bool online,
                            uint8_t* output, size_t* length, uint32_t* count);
int h2_alipay_transit_update(const h2_alipay_client_config_t* config,
                             h2_alipay_transit_t* state, const char* number,
                             const char* type);
int h2_alipay_transit_status(const h2_alipay_client_config_t* config,
                             const char* type, bool* exists, int32_t* expiry,
                             int32_t* remaining);
int h2_alipay_transit_generate(const h2_alipay_client_config_t* config,
                               h2_alipay_transit_t* state, const char* number,
                               const char* type, uint8_t* output,
                               size_t* length);
int h2_alipay_transit_last(const h2_alipay_client_config_t* config,
                           h2_alipay_transit_t* state,
                           h2_alipay_transit_card_t* output);
int h2_alipay_transit_discard(const h2_alipay_client_config_t* config);
#endif
