#ifndef H2_ALIPAY_PAYMENT_MODEL_H
#define H2_ALIPAY_PAYMENT_MODEL_H
#include "h2_alipay_payment.h"
#define H2_ALIPAY_PAYMENT_ATTEMPTS 3u

typedef struct h2_alipay_payment_attempt {
  char code[H2_ALIPAY_PAYMENT_CODE_CAPACITY];
  uint64_t created_ms;
  uint32_t id;
} h2_alipay_payment_attempt_t;

typedef struct h2_alipay_payment_model {
  char visible[H2_ALIPAY_PAYMENT_CODE_CAPACITY];
  size_t length;
  uint64_t refresh_ms, query_ms;
  uint32_t next_id;
  size_t query_cursor;
  h2_alipay_payment_attempt_t attempts[H2_ALIPAY_PAYMENT_ATTEMPTS];
} h2_alipay_payment_model_t;

void h2_alipay_payment_model_clear(h2_alipay_payment_model_t* model);
int h2_alipay_payment_model_code(h2_alipay_payment_model_t* model,
                                 const uint8_t* code, size_t length,
                                 uint64_t now);
/** Select a bounded, unexpired attempt and copy its original displayed code.
 * Caller invokes only with no operation in flight; expiry never destroys the
 * borrowed code of an executing query. Caller owns/wipes the returned copy. */
int h2_alipay_payment_model_query(h2_alipay_payment_model_t* model,
                                  uint64_t now,
                                  h2_alipay_payment_attempt_t* out);
bool h2_alipay_payment_model_has(const h2_alipay_payment_model_t* model,
                                 uint32_t id);
/** Extract the exact current SDK BLE result service payload from complete AD
 * data. Accept only UUID FA38 (16 or Bluetooth-base 128-bit) and 27 bytes. */
int h2_alipay_payment_ble_result(const uint8_t* adv, size_t length,
                                 uint8_t out[27]);
#endif
