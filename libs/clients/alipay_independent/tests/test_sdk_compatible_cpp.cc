#include "alipay_account_manage.h"
#include "alipay_bind.h"
#include "alipay_nfc_offline_pay.h"
#include "alipay_pay.h"
#include "alipay_pay_result.h"

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

int main() {
  assert(alipay_pre_init() != ALIPAY_RV_OK);
  assert(!alipay_get_binding_status());
  binding_status_e status = ALIPAY_STATUS_BINDING_OK;
  assert(alipay_query_binding_result(&status) != ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_BINDING_FAIL);
  return 0;
}
