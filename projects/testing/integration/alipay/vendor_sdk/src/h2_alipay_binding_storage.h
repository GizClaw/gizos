#ifndef H2_ALIPAY_BINDING_STORAGE_H
#define H2_ALIPAY_BINDING_STORAGE_H

#include "h2_alipay.h"
#include "h2_alipay_storage.h"

enum {
  H2_ALIPAY_SDK_ITEM_SHARED_KEY = 1,
  H2_ALIPAY_SDK_ITEM_SEED = 2,
  H2_ALIPAY_SDK_ITEM_TID = 6,
  H2_ALIPAY_SDK_ITEM_UID = 7,
};

void h2_alipay_binding_value_digest(const uint8_t* data, size_t length,
                                    uint8_t out[32]);
int h2_alipay_binding_storage_read(const h2_alipay_storage_t* storage,
                                   h2_alipay_binding_result_t* out,
                                   uint8_t digest[32]);
int h2_alipay_binding_storage_verify(const h2_alipay_storage_t* storage,
                                     const uint8_t digest[32]);
int h2_alipay_binding_storage_mark(const h2_alipay_storage_t* storage,
                                   const uint8_t digest[32]);
int h2_alipay_binding_storage_invalidate(const h2_alipay_storage_t* storage);
int h2_alipay_binding_storage_probe(const h2_alipay_storage_t* storage,
                                    bool* record_present);

#endif
