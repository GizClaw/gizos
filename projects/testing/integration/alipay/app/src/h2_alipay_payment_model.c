#include "h2_alipay_payment_model.h"
#include <string.h>

static void wipe(void* data, size_t length) {
  volatile uint8_t* p = data;
  while (length-- != 0u) *p++ = 0u;
}
void h2_alipay_payment_model_clear(h2_alipay_payment_model_t* model) {
  if (model != NULL) wipe(model, sizeof(*model));
}
static void expire(h2_alipay_payment_model_t* model, uint64_t now) {
  for (size_t i = 0u; i < H2_ALIPAY_PAYMENT_ATTEMPTS; ++i)
    if (model->attempts[i].id != 0u && now >= model->attempts[i].created_ms &&
        now - model->attempts[i].created_ms >= 120000u)
      wipe(&model->attempts[i], sizeof(model->attempts[i]));
}
int h2_alipay_payment_model_code(h2_alipay_payment_model_t* model,
                                 const uint8_t* code, size_t length,
                                 uint64_t now) {
  if (model == NULL || code == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (length != 18u && length != 19u) return H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; i < length; ++i)
    if (code[i] < '0' || code[i] > '9') return H2_PAL_ERR_FORMAT;
  expire(model, now);
  size_t slot = H2_ALIPAY_PAYMENT_ATTEMPTS;
  for (size_t i = 0u; i < H2_ALIPAY_PAYMENT_ATTEMPTS; ++i) {
    if (model->attempts[i].id != 0u &&
        strlen(model->attempts[i].code) == length &&
        memcmp(model->attempts[i].code, code, length) == 0) {
      slot = i;
      break;
    }
    if (slot == H2_ALIPAY_PAYMENT_ATTEMPTS && model->attempts[i].id == 0u)
      slot = i;
  }
  if (slot == H2_ALIPAY_PAYMENT_ATTEMPTS) return H2_PAL_ERR_BUSY;
  if (model->attempts[slot].id == 0u) {
    if (++model->next_id == 0u) ++model->next_id;
    model->attempts[slot].id = model->next_id;
    model->attempts[slot].created_ms = now;
    memcpy(model->attempts[slot].code, code, length);
  }
  wipe(model->visible, sizeof(model->visible));
  memcpy(model->visible, code, length);
  model->length = length;
  model->refresh_ms = now + 60000u;
  return 0;
}
int h2_alipay_payment_model_query(h2_alipay_payment_model_t* model,
                                  uint64_t now,
                                  h2_alipay_payment_attempt_t* out) {
  if (model == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  wipe(out, sizeof(*out));
  expire(model, now);
  if (now < model->query_ms) return H2_PAL_ERR_WOULD_BLOCK;
  for (size_t i = 0u; i < H2_ALIPAY_PAYMENT_ATTEMPTS; ++i) {
    size_t slot = (model->query_cursor + i) % H2_ALIPAY_PAYMENT_ATTEMPTS;
    if (model->attempts[slot].id != 0u) {
      *out = model->attempts[slot];
      model->query_cursor = (slot + 1u) % H2_ALIPAY_PAYMENT_ATTEMPTS;
      model->query_ms = now + 2000u;
      return 0;
    }
  }
  return H2_PAL_ERR_NOT_FOUND;
}
bool h2_alipay_payment_model_has(const h2_alipay_payment_model_t* model,
                                 uint32_t id) {
  if (model == NULL || id == 0u) return false;
  for (size_t i = 0u; i < H2_ALIPAY_PAYMENT_ATTEMPTS; ++i)
    if (model->attempts[i].id == id) return true;
  return false;
}
