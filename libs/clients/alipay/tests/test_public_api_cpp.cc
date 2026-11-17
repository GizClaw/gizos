#include "h2_alipay_api.h"
#include "h2_alipay_scan.h"
#include "h2_alipay_tap.h"

extern "C" {}

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
static_assert(
    std::is_same<decltype(&alipay_migrate_data), retval_e (*)(void)>::value,
    "Original legacy migration signature");
static_assert(std::is_same<decltype(&alipay_biz_process_entry),
                           retval_e (*)(const uint8_t*, uint32_t, uint8_t**,
                                        uint32_t*)>::value,
              "Original synchronization signature");
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
static_assert(std::is_same<decltype(&alipay_get_collectioncode),
                           retval_e (*)(uint8_t*, uint32_t*)>::value,
              "Original collection code signature");
static_assert(std::is_same<decltype(&alipay_query_remote_bind_status),
                           retval_e (*)(alipay_remote_bind_status_t*)>::value,
              "Original remote binding signature");
static_assert(std::is_same<decltype(&alipay_query_order_result),
                           retval_e (*)(char*, alipay_payment_status_e*,
                                        char (*)[14])>::value,
              "Original bitmap result signature");
static_assert(std::is_same<decltype(&alipay_get_balance_bmp),
                           retval_e (*)(char (*)[14])>::value,
              "Original balance bitmap signature");
static_assert(
    std::is_same<decltype(&alipay_query_income_details),
                 retval_e (*)(char*, alipay_income_detail_info_t*)>::value,
    "Original income signature");
static_assert(
    std::is_same<decltype(&alipay_query_flow_break), void (*)(void)>::value,
    "Original query cancellation signature");
static_assert(sizeof(alipay_income_detail_info_t) == 705u,
              "Original income layout");
static_assert(std::is_same<decltype(&alipay_dzs_get_precheck_status),
                           retval_e (*)(uint8_t*)>::value,
              "Original DZS permission signature");
static_assert(
    std::is_same<decltype(&alipay_dzs_generate_payment_qrcode),
                 retval_e (*)(const char*, alipay_dzs_code_info_t*)>::value,
    "Original DZS generation signature");
static_assert(std::is_same<decltype(&alipay_dzs_query_latest_payment_result),
                           retval_e (*)(alipay_payment_status_e*)>::value,
              "Original DZS query signature");
static_assert(std::is_same<decltype(&alipay_dzs_cancel_latest_payment),
                           void (*)(void)>::value,
              "Original DZS cancellation signature");
static_assert(sizeof(alipay_dzs_code_info_t) == 1032u,
              "Original DZS code layout");
static_assert(std::is_same<decltype(&alipay_query_order_result_string),
                           retval_e (*)(char*, alipay_payment_status_e*, char*,
                                        char*, uint32_t*)>::value,
              "Original result string signature");
static_assert(sizeof(alipay_payment_status_e) == sizeof(int32_t),
              "Original result status ABI");

using text_getter_t = retval_e (*)(uint8_t*, uint32_t*);
static_assert(
    std::is_same<decltype(&alipay_get_logon_ID), text_getter_t>::value,
    "Original login getter signature");
static_assert(
    std::is_same<decltype(&alipay_get_nick_name), text_getter_t>::value,
    "Original nickname getter signature");
static_assert(
    std::is_same<decltype(&alipay_get_privacy_code), text_getter_t>::value,
    "Original privacy URL signature");
static_assert(
    std::is_same<decltype(&alipay_get_setting_code), text_getter_t>::value,
    "Original settings URL signature");
static_assert(std::is_same<decltype(&alipay_npay_get_coupon_detail_url),
                           text_getter_t>::value,
              "Original coupon URL signature");
static_assert(
    std::is_same<decltype(&alipay_get_nfc_available_nearby_locations_url),
                 text_getter_t>::value,
    "Original nearby URL signature");
static_assert(std::is_same<decltype(&alipay_nfc_init_callbacks),
                           void (*)(alipay_nfc_payment_callback_t)>::value,
              "Original NFC callback registration signature");
static_assert(std::is_same<decltype(&alipay_nfc_setup_multi_arch),
                           retval_e (*)(uint8_t)>::value,
              "Original NFC architecture signature");
static_assert(std::is_same<decltype(&alipay_npay_set_quick_pay_flag),
                           retval_e (*)(bool)>::value,
              "Original quick payment setter signature");
static_assert(std::is_same<decltype(&alipay_npay_get_quick_pay_flag),
                           bool (*)(void)>::value &&
                  std::is_same<decltype(&alipay_npay_get_quick_pay_show_flag),
                               bool (*)(void)>::value,
              "Original quick payment getter signatures");
static_assert(std::is_same<decltype(&alipay_npay_need_show_quick_open_guide),
                           bool (*)(void)>::value,
              "Original quick payment guide signature");
static_assert(std::is_same<decltype(&alipay_npay_set_ble_mtu_size),
                           void (*)(uint32_t)>::value,
              "Original NFC BLE MTU signature");
static_assert(std::is_same<decltype(&alipay_npay_ble_broadcast_init),
                           retval_e (*)(void)>::value,
              "Original NFC static cipher initialization signature");
static_assert(
    std::is_same<decltype(&alipay_get_paycode_with_nfc_token),
                 retval_e (*)(uint8_t*, uint32_t, uint8_t*, uint32_t*)>::value,
    "Original NFC static ciphertext signature");
static_assert(std::is_same<decltype(&alipay_nfc_test_token_generation),
                           char* (*)(void)>::value,
              "Original NFC debug token signature");
static_assert(std::is_same<decltype(&alipay_direct_provision),
                           retval_e (*)(const char*)>::value &&
                  std::is_same<decltype(&alipay_provision_ota),
                               decltype(&alipay_direct_provision)>::value &&
                  std::is_same<decltype(&alipay_active_device),
                               decltype(&alipay_direct_provision)>::value,
              "Original provisioning and device activation signatures");
static_assert(
    std::is_same<decltype(&alipay_transit_get_activity_status_offline),
                 retval_e (*)(bool*)>::value &&
        std::is_same<decltype(&alipay_transit_get_activity_status_online),
                     retval_e (*)(bool*)>::value,
    "Original transit activity signatures");
static_assert(
    std::is_same<decltype(&alipay_transit_get_card_list_offline),
                 retval_e (*)(alipay_tansit_CardBaseVO_t*, uint32_t*,
                              uint32_t*)>::value &&
        std::is_same<decltype(&alipay_transit_get_card_list_online),
                     decltype(&alipay_transit_get_card_list_offline)>::value,
    "Original transit list signatures");
static_assert(std::is_same<decltype(&alipay_transit_update_card_data),
                           retval_e (*)(char*, char*, char*, uint32_t)>::value,
              "Original transit update signature");
static_assert(std::is_same<decltype(&alipay_transit_check_card_status),
                           retval_e (*)(char*, char*,
                                        alipay_transit_card_status_t*)>::value,
              "Original transit status signature");
static_assert(std::is_same<decltype(&alipay_transit_get_TransitCode),
                           retval_e (*)(char*, char*, uint8_t*, uint32_t*,
                                        char*, uint32_t)>::value,
              "Original transit code signature");
static_assert(std::is_same<decltype(&alipay_transit_get_the_last_transitCode),
                           retval_e (*)(char*, uint8_t*, uint32_t*, char*,
                                        uint32_t)>::value,
              "Original latest transit code signature");
static_assert(
    std::is_same<decltype(&alipay_transit_get_the_last_transcode_info),
                 retval_e (*)(alipay_tansit_CardBaseVO_t*)>::value,
    "Original latest transit card signature");
static_assert(
    std::is_same<decltype(&alipay_init_transit_module), int (*)(void)>::value &&
        std::is_same<decltype(&alipay_break_rpc), void (*)(void)>::value &&
        std::is_same<decltype(&alipay_get_last_card_list_result),
                     void (*)(transit_card_list_result_e*)>::value &&
        std::is_same<decltype(&alipay_get_last_transit_code_result),
                     void (*)(transit_transit_code_result_e*)>::value,
    "Original transit lifecycle and result signatures");
static_assert(sizeof(alipay_tansit_CardBaseVO_t) == 120u &&
                  sizeof(alipay_transit_card_status_t) == 12u,
              "Original transit buffer layouts");
static_assert(std::is_same<decltype(&alipay_nfc_recv_data_handle),
                           int (*)(const uint8_t*, uint32_t)>::value &&
                  std::is_same<decltype(&alipay_npay_ble_recv_data_handle),
                               int (*)(const uint8_t*, uint32_t)>::value,
              "Original NFC and connected BLE reception signatures");
static_assert(std::is_same<decltype(&alipay_nfc_recv_data_handle_with_context),
                           int (*)(alipay_nfc_pay_reader_context_t,
                                   const uint8_t*, uint32_t, uint8_t*)>::value,
              "Original contextual NFC reception signature");
static_assert(std::is_same<decltype(&alipay_set_nfc_write_result_with_nfc_data),
                           retval_e (*)(uint8_t*, uint32_t, uint8_t)>::value,
              "Original NFC write-result setter signature");
static_assert(
    std::is_same<decltype(&alipay_get_nfc_write_result_with_nfc_token),
                 retval_e (*)(uint8_t*, uint32_t, uint8_t*)>::value,
    "Original NFC write-result getter signature");
static_assert(std::is_same<decltype(&alipay_query_last_nfc_pay_result),
                           retval_e (*)(alipay_payment_status_e*, char*, char*,
                                        uint32_t*)>::value,
              "Original latest NFC payment query signature");
static_assert(
    std::is_same<decltype(&alipay_union_code_parse_with_code),
                 retval_e (*)(char*, alipay_union_code_order_info_t*)>::value,
    "Original union parser signature");
static_assert(
    std::is_same<decltype(&alipay_union_code_parse_with_img),
                 retval_e (*)(uint8_t*, uint32_t, uint32_t,
                              alipay_union_code_order_info_t*)>::value,
    "Original NV12 image parser signature");
static_assert(std::is_same<decltype(&alipay_nfc_parse_with_content),
                           decltype(&alipay_union_code_parse_with_code)>::value,
              "Original NFC parser signature");
static_assert(std::is_same<decltype(&alipay_nfc_confirm_payment),
                           decltype(&alipay_union_code_confirm_payment)>::value,
              "Original NFC confirmation signature");
static_assert(
    std::is_same<decltype(&alipay_unbinding), retval_e (*)(void)>::value,
    "Original unbind signature");
static_assert(
    std::is_same<decltype(&alipay_reset_all), retval_e (*)(void)>::value,
    "Original reset signature");

static_assert(std::is_same<decltype(&alipay_binding_process_notify),
                           void (*)(int)>::value,
              "Original binding-progress callback signature");
static_assert(std::is_same<decltype(&ALIPAY_deprecated_storage_read),
                           int32_t (*)(uint32_t, uint8_t*, uint32_t*)>::value,
              "Original legacy item reader signature");
static_assert(
    std::is_same<decltype(&ALIPAY_deprecated_storage_write),
                 int32_t (*)(uint32_t, const uint8_t*, uint32_t)>::value,
    "Original legacy item writer signature");
static_assert(std::is_same<decltype(&ALIPAY_deprecated_storage_delete),
                           int32_t (*)(uint32_t)>::value &&
                  std::is_same<decltype(&ALIPAY_deprecated_storage_exists),
                               int32_t (*)(uint32_t)>::value,
              "Original legacy item delete/existence signatures");

int main() {
  alipay_binding_process_notify(10);
  assert(ALIPAY_deprecated_storage_read(4u, nullptr, nullptr) != 0);
  assert(ALIPAY_deprecated_storage_write(4u, nullptr, 1u) != 0);
  assert(ALIPAY_deprecated_storage_delete(4u) != 0);
  assert(ALIPAY_deprecated_storage_exists(4u) != 0);
  alipay_nfc_init_callbacks({});
  assert(alipay_nfc_setup_multi_arch(1u) == ALIPAY_RV_OK);
  assert(alipay_nfc_setup_multi_arch(0u) == ALIPAY_RV_OK);
  assert(!alipay_npay_get_quick_pay_flag());
  assert(!alipay_npay_get_quick_pay_show_flag());
  assert(alipay_npay_set_quick_pay_flag(true) != ALIPAY_RV_OK);
  assert(!alipay_npay_need_show_quick_open_guide());
  alipay_npay_set_ble_mtu_size(101u);
  assert(alipay_nfc_test_token_generation() == nullptr);
  assert(alipay_npay_ble_broadcast_init() != ALIPAY_RV_OK);
  assert(alipay_get_paycode_with_nfc_token(nullptr, 0u, nullptr, nullptr) ==
         99);
  assert(alipay_set_nfc_write_result_with_nfc_data(nullptr, 0u, 1u) == 99);
  assert(alipay_get_nfc_write_result_with_nfc_token(nullptr, 0u, nullptr) ==
         99);
  assert(alipay_pre_init() != ALIPAY_RV_OK);
  assert(alipay_biz_process_entry(nullptr, 1u, nullptr, nullptr) ==
         ALIPAY_RV_WRONG_PARAM);
  assert(alipay_migrate_data() != ALIPAY_RV_OK);
  assert(!alipay_get_binding_status());
  uint8_t text[24];
  uint32_t length = sizeof(text);
  assert(alipay_get_companyName(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_productModel(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_productType() == UINT32_MAX);
  assert(alipay_get_sn(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_imei(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_mac(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_logon_ID(text, &length) != ALIPAY_RV_OK);
  assert(alipay_get_nick_name(text, &length) != ALIPAY_RV_OK);
  assert(alipay_unbinding() != ALIPAY_RV_OK);
  assert(alipay_reset_all() != ALIPAY_RV_OK);
  uint8_t url[256] = {};
  length = sizeof(url);
  assert(alipay_get_privacy_code(url, &length) == ALIPAY_RV_OK);
  assert(length == 82u && url[length] == 0u);
  length = sizeof(url);
  assert(alipay_get_setting_code(url, &length) == ALIPAY_RV_OK);
  assert(length == 83u && url[length] == 0u);
  binding_status_e status = ALIPAY_STATUS_BINDING_OK;
  assert(alipay_query_binding_result(&status) != ALIPAY_RV_OK);
  assert(status == ALIPAY_STATUS_BINDING_FAIL);
  uint8_t payer[20];
  uint32_t payer_length = sizeof(payer);
  assert(alipay_get_paycode(payer, &payer_length) != ALIPAY_RV_OK);
  assert(alipay_get_collectioncode(payer, &payer_length) != ALIPAY_RV_OK);
  alipay_remote_bind_status_t remote = REMOTE_STATUS_BIND;
  assert(alipay_query_remote_bind_status(&remote) != ALIPAY_RV_OK);
  assert(alipay_query_remote_bind_status(nullptr) == ALIPAY_RV_WRONG_PARAM);
  alipay_payment_status_e payment = ALIPAY_PAYMENT_STATUS_SUCCESS;
  char amount[32], ext[32];
  char bitmap[32][14] = {};
  assert(alipay_get_balance_bmp(bitmap) != ALIPAY_RV_OK);
  uint32_t ext_length = sizeof(ext);
  assert(alipay_query_last_nfc_pay_result(&payment, amount, ext, &ext_length) !=
         ALIPAY_RV_OK);
  char code[] = "284960766182755224";
  assert(alipay_query_order_result(code, &payment, bitmap) != ALIPAY_RV_OK);
  alipay_income_detail_info_t income = {};
  assert(alipay_query_income_details(code, &income) != ALIPAY_RV_OK);
  alipay_query_flow_break();
  uint8_t enabled = 0xa5u;
  assert(alipay_dzs_get_precheck_status(&enabled) != ALIPAY_RV_OK);
  alipay_dzs_code_info_t dzs = {};
  assert(alipay_dzs_generate_payment_qrcode("0.01", &dzs) != ALIPAY_RV_OK);
  assert(alipay_dzs_query_latest_payment_result(&payment) != ALIPAY_RV_OK);
  alipay_dzs_cancel_latest_payment();
  alipay_union_code_order_info_t order = {};
  alipay_union_code_pay_result_t scan_payment = {};
  assert(alipay_union_code_parse_with_code(code, &order) != ALIPAY_RV_OK);
  assert(alipay_nfc_parse_with_content(code, &order) != ALIPAY_RV_OK);
  assert(alipay_nfc_url_parse(code, &order) != ALIPAY_RV_OK);
  assert(alipay_union_code_confirm_payment(&order, &scan_payment) !=
         ALIPAY_RV_OK);
  assert(alipay_nfc_confirm_payment(&order, &scan_payment) != ALIPAY_RV_OK);
  alipay_npay_ble_pay_result_t tap = {};
  assert(alipay_npay_ble_scan_pay_res_data_handle_v2(nullptr, 27u, &tap) ==
         ALIPAY_RV_WRONG_PARAM);
  uint32_t cents = 0u;
  assert(alipay_npay_ble_scan_pay_res_data_handle(nullptr, 27u, &cents) ==
         ALIPAY_RV_WRONG_PARAM);
  length = sizeof(url);
  assert(alipay_npay_get_coupon_detail_url(url, &length) != ALIPAY_RV_OK);
  assert(alipay_get_nfc_available_nearby_locations_url(url, &length) ==
         ALIPAY_RV_OK);
  assert(alipay_get_plain_paycode_with_nfc_token(payer, 18u, url, &length) !=
         ALIPAY_RV_OK);
  char aid[256];
  length = sizeof(aid);
  assert(alipay_get_aid_code(aid, &length, false) != ALIPAY_RV_OK);
  assert(alipay_query_order_result_string(code, &payment, amount, ext,
                                          &ext_length) != ALIPAY_RV_OK);
  assert(payment == ALIPAY_PAYMENT_STATUS_UNKNOWN);
  return 0;
}
