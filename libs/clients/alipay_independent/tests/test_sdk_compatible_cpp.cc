#include "alipay_account_manage.h"
#include "alipay_bind.h"
#include "alipay_nfc_offline_pay.h"
#include "alipay_pay.h"
#include "alipay_pay_result.h"
#include "vendor_device_info.h"

#include <cassert>
#include <type_traits>

static_assert(std::is_same<decltype(&alipay_pre_init), int (*)(void)>::value,
              "Original initialization signature");
static_assert(std::is_same<decltype(&alipay_get_binding_code),
                           retval_e (*)(uint32_t, char*, int*)>::value,
              "Original binding-code signature");
static_assert(std::is_same<decltype(&alipay_query_binding_result),
                           retval_e (*)(binding_status_e*)>::value,
              "Original binding-result signature");
static_assert(sizeof(retval_e) == 4u, "Original result ABI");
static_assert(std::is_same<decltype(&alipay_get_companyName),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original company getter signature");
static_assert(std::is_same<decltype(&alipay_get_productModel),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original model getter signature");

static_assert(
    std::is_same<decltype(&alipay_get_productType), uint32_t (*)(void)>::value,
    "Original product type signature");
static_assert(std::is_same<decltype(&alipay_get_sn),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original SN signature");
static_assert(std::is_same<decltype(&alipay_get_imei),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original IMEI signature");
static_assert(std::is_same<decltype(&alipay_get_mac),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original MAC signature");

static_assert(std::is_same<decltype(&alipay_get_paycode),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original payer code signature");
static_assert(std::is_same<decltype(&alipay_query_order_result_string),
                           retval_e (*)(char*, alipay_payment_status_e*, char*,
                                        char*, uint32_t*)>::value,
              "Original result string signature");
static_assert(sizeof(alipay_payment_status_e) == sizeof(int32_t),
              "Original result status ABI");

int main() {
  assert(alipay_pre_init() != ALIPAY_RV_OK);
  assert(!alipay_get_binding_status());
  uint8_t text[24];
  uint32_t length = sizeof(text);
  assert(alipay_get_companyName(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_productModel(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_productType() == UINT32_MAX);
  assert(alipay_get_sn(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_imei(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_mac(text, &length) != ALIPAY_RV_OK);
  binding_status_e status = ALIPAY_STATUS_BINDING_OK;
  assert(alipay_query_binding_result(&status) != ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_BINDING_FAIL);
  uint8_t payer[20];
  uint32_t payer_length = sizeof(payer);
  assert(alipay_get_paycode(payer, &payer_length) != ALIPAY_RV_OK);
  alipay_payment_status_e payment = ALIPAY_PAYMENT_STATUS_SUCCESS;
  char amount[32], ext[32];
  uint32_t ext_length = sizeof(ext);
  char code[] = "284960766182755224";
  assert(alipay_query_order_result_string(code, &payment, amount, ext,
                                          &ext_length) != ALIPAY_RV_OK);
  assert(payment == ALIPAY_PAYMENT_STATUS_UNKNOWN);
  return 0;
}
