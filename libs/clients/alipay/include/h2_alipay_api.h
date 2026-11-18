#ifndef H2_ALIPAY_API_H
#define H2_ALIPAY_API_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Public compatibility ABI implemented by this package. Configure borrowed PAL
 * dependencies through h2_alipay_context.h before stateful calls. Business
 * operations are synchronous; callers serialize them through completion,
 * including callbacks. Radio acquisition and transmission belong to callers.
 * Detailed ownership and cancellation contracts are in the configuration API.
 */
typedef int32_t retval_e;
typedef int32_t binding_status_e;

#define ALIPAY_RV_OK 0
#define ALIPAY_RV_WRONG_PARAM 19
#define ALIPAY_RV_HAL_ERROR 57
#define FEATURE_PAYCODE 0x0001
#define FEATURE_TRANSITCODE 0x0002
#define FEATURE_NFCPAY 0x0004
#define FEATURE_BLEPAY 0x0008
#define FEATURE_ANTFOREST 0x0010
#define FEATURE_ELLIPSE 0x2000
#define FEATURE_SQUARE 0x4000
#define FEATURE_CIRCLE 0x8000
#define ALIPAY_STATUS_BINDING_FAIL 0
#define ALIPAY_STATUS_UNBINDED 1
#define ALIPAY_STATUS_START_BINDING 2
#define ALIPAY_STATUS_BINDING_OK 0xa5
#define ALIPAY_STATUS_UNKNOWN 0xff

typedef enum {
  ALIPAY_PAYMENT_STATUS_SUCCESS = 0,
  ALIPAY_PAYMENT_STATUS_FAIL = 1,
  ALIPAY_PAYMENT_STATUS_NOTHING = 2,
  ALIPAY_PAYMENT_STATUS_UNBIND_BY_USER = 3,
  ALIPAY_PAYMENT_STATUS_DISABLED_BY_USER = 4,
  ALIPAY_PAYMENT_STATUS_UNKNOWN = 5,
  ALIPAY_PAYMENT_STATUS_PROCESSING = 6,
} alipay_payment_status_e;

typedef enum {
  ALIPAY_FAIL_RES_AMOUNT_LIMIT = 0,
  ALIPAY_FAIL_RES_BALANCE_NOT_ENOUGH = 1,
  ALIPAY_FAIL_RES_COMMON_FAIL = 2,
  ALIPAY_FAIL_RES_ACCOUNT_NOT_EXIST = 3,
  ALIPAY_FAIL_RES_CASE_NOT_SUPPORT = 4,
} alipay_scan_pay_fail_res_e;

typedef enum {
  REMOTE_STATUS_BIND = 0,
  REMOTE_STATUS_UNBIND = 1,
  REMOTE_STATUS_UNKNOWN = 2,
} alipay_remote_bind_status_t;

int alipay_pre_init(void);
retval_e alipay_get_binding_code(const uint32_t feature, char* result,
                                 int* len_result);
retval_e alipay_query_binding_result(binding_status_e* status);
void alipay_binding_process_notify(int status);
bool alipay_get_binding_status(void);
retval_e alipay_query_remote_bind_status(alipay_remote_bind_status_t* status);
retval_e alipay_get_logon_ID(uint8_t* id, uint32_t* len_id);
retval_e alipay_get_nick_name(uint8_t* nick_name, uint32_t* len_nick_name);
retval_e alipay_unbinding(void);
retval_e alipay_reset_all(void);
retval_e alipay_get_privacy_code(uint8_t* code, uint32_t* length);
retval_e alipay_get_setting_code(uint8_t* code, uint32_t* length);
retval_e alipay_get_aid_code(char aid_code[256], uint32_t* length,
                             bool supports_ble);
retval_e alipay_get_balance_bmp(char bitmap[32][14]);
retval_e alipay_get_paycode(uint8_t* code, uint32_t* length);
retval_e alipay_get_collectioncode(uint8_t* code, uint32_t* length);

typedef struct {
  bool is_success;
  char bitmap_amount[32][14];
  char message[256];
} alipay_income_detail_info_t;

retval_e alipay_query_order_result(char* code, alipay_payment_status_e* status,
                                   char bitmap_amount[32][14]);
retval_e alipay_query_order_result_string(char* code,
                                          alipay_payment_status_e* status,
                                          char amount[32], char* ext_data,
                                          uint32_t* ext_data_length);
retval_e alipay_query_income_details(char* code,
                                     alipay_income_detail_info_t* detail);
void alipay_query_flow_break(void);
retval_e alipay_query_last_nfc_pay_result(alipay_payment_status_e* status,
                                          char amount[32], char* ext_data,
                                          uint32_t* ext_data_length);

/** Reply application bytes are borrowed only for the callback. Pass the
 * payload and length unchanged to the caller-owned NFC write path. Framing
 * and transport completion follow the terminal's qualified protocol.
 * SDK processing status ignores the callback result; track transmission
 * errors separately. SDK success does not establish a paid transaction. */
typedef int (*alipay_nfc_send_data_handle_t)(const uint8_t* data,
                                             uint32_t length);
typedef int (*alipay_ble_broadcast_data_handle_t)(const uint8_t* data,
                                                  uint32_t length);
typedef int (*alipay_ble_notify_data_handle_t)(bool first, bool finish,
                                               const uint8_t* data,
                                               uint32_t length);
typedef int (*alipay_ble_broadcast_direct_pay_data_handle_t)(
    int manufacturer, const uint8_t* data, uint32_t length,
    const uint8_t* service_uuid, uint32_t service_uuid_length,
    uint8_t quick_pay);
typedef int (*alipay_nfc_quick_pay_flag_change_callback_t)(bool quick_pay);

typedef enum {
  NFC_PAYMENT_STATUS_START = 0,
  NFC_PAYMENT_STATUS_SUCCESS,
  NFC_PAYMENT_STATUS_FAIL,
} alipay_nfc_pay_status_t;

typedef enum {
  NFC_PAYMENT_FAIL_ERROR_SUCCESS = 0,
  NFC_PAYMENT_FAIL_ERROR_NOT_BIND,
  NFC_PAYMENT_FAIL_ERROR_UNSUPPORT_WITHOUT_NETWORK,
  NFC_PAYMENT_FAIL_ERROR_HUOGUI_TAG,
  NFC_PAYMENT_FAIL_ERROR_UNKNOWN_TAG,
  NFC_PAYMENT_FAIL_ERROR_OTHER,
} alipay_nfc_pay_error_t;

typedef struct {
  alipay_nfc_pay_status_t status;
  alipay_nfc_pay_error_t code;
  char amount[64];
} alipay_payment_status_t;

typedef int (*alipay_nfc_notify_payment_result_handle_t)(
    alipay_payment_status_t* status);

typedef struct {
  alipay_nfc_send_data_handle_t send_data_handle;
  alipay_ble_broadcast_data_handle_t ble_broadcast_data_handle;
  alipay_ble_notify_data_handle_t ble_notify_data_handle;
  alipay_nfc_notify_payment_result_handle_t notify_payment_result_handle;
  alipay_ble_broadcast_direct_pay_data_handle_t
      ble_broadcast_direct_pay_data_handle;
  alipay_nfc_quick_pay_flag_change_callback_t quick_pay_flag_change_callback;
} alipay_nfc_payment_callback_t;

typedef enum {
  NFC_PAY_READER_CONTEXT_WITHIN_ALIPAY_WALLET = 0,
  NFC_PAY_READER_CONTEXT_AWAKE,
  NFC_PAY_READER_CONTEXT_SLEEP,
  NFC_PAY_READER_CONTEXT_LOCK_SCREEN,
  NFC_PAY_READER_CONTEXT_OTHER,
} alipay_nfc_pay_reader_context_t;

retval_e alipay_npay_ble_broadcast_init(void);
void alipay_nfc_init_callbacks(alipay_nfc_payment_callback_t callbacks);
int alipay_nfc_recv_data_handle(const uint8_t* data, uint32_t length);
int alipay_nfc_recv_data_handle_with_context(
    alipay_nfc_pay_reader_context_t context, const uint8_t* data,
    uint32_t length, uint8_t amount[16]);
int alipay_npay_ble_recv_data_handle(const uint8_t* data, uint32_t length);
void alipay_npay_set_ble_mtu_size(uint32_t mtu);
char* alipay_nfc_test_token_generation(void);
bool alipay_npay_get_quick_pay_show_flag(void);
bool alipay_npay_get_quick_pay_flag(void);
retval_e alipay_npay_set_quick_pay_flag(bool flag);
bool alipay_npay_need_show_quick_open_guide(void);

/** Ciphertext output retains the compatibility ABI's output-only length.
 * Callers provide at least 44 bytes for a normal 18/19-digit payer code. */
retval_e alipay_get_paycode_with_nfc_token(uint8_t* token,
                                           uint32_t token_length,
                                           uint8_t code[32],
                                           uint32_t* code_length);
retval_e alipay_get_plain_paycode_with_nfc_token(uint8_t* token,
                                                 uint32_t token_length,
                                                 uint8_t code[32],
                                                 uint32_t* code_length);

typedef struct {
  uint8_t pay_result_type;
  uint8_t coupon_status;
  uint32_t order_amount_fen;
  uint32_t real_pay_amount_fen;
} alipay_npay_ble_pay_result_t;

retval_e alipay_npay_ble_scan_pay_res_data_handle_v2(
    const uint8_t* data, uint32_t length, alipay_npay_ble_pay_result_t* result);
retval_e alipay_npay_ble_scan_pay_res_data_handle(const uint8_t* data,
                                                  uint32_t length,
                                                  uint32_t* amount_fen);
retval_e alipay_npay_get_coupon_detail_url(uint8_t url[256], uint32_t* length);
retval_e alipay_set_nfc_write_result_with_nfc_data(uint8_t* data,
                                                   uint32_t length,
                                                   uint8_t success);
retval_e alipay_get_nfc_write_result_with_nfc_token(uint8_t* token,
                                                    uint32_t length,
                                                    uint8_t* success);
retval_e alipay_get_nfc_available_nearby_locations_url(uint8_t url[256],
                                                       uint32_t* length);
retval_e alipay_nfc_setup_multi_arch(uint8_t multi_arch);

typedef struct {
  bool is_success;
  alipay_scan_pay_fail_res_e err_code;
  char qr_code[1024];
} alipay_dzs_code_info_t;

retval_e alipay_dzs_get_precheck_status(uint8_t* status);
retval_e alipay_dzs_generate_payment_qrcode(const char* amount,
                                            alipay_dzs_code_info_t* code);
void alipay_dzs_cancel_latest_payment(void);
retval_e alipay_dzs_query_latest_payment_result(
    alipay_payment_status_e* status);

typedef struct {
  bool is_success;
  char err_code[8];
  char payee_name[128];
  char payee_logo[1024];
  char pay_amount[32];
} alipay_union_code_order_info_t;

typedef enum {
  ALIPAY_SCAN_PAYMENT_STATUS_SUCCESS = 0,
  ALIPAY_SCAN_PAYMENT_STATUS_FAIL = 1,
  ALIPAY_SCAN_PAYMENT_STATUS_NOTHING = 2,
  ALIPAY_SCAN_PAYMENT_STATUS_UNBIND_BY_USER = 3,
  ALIPAY_SCAN_PAYMENT_STATUS_DISABLED_BY_USER = 4,
  ALIPAY_SCAN_PAYMENT_STATUS_UNKNOWN = 5,
  ALIPAY_SCAN_PAYMENT_STATUS_PROCESSING = 6,
} alipay_scan_payment_status_e;

typedef struct {
  alipay_scan_payment_status_e status;
  alipay_scan_pay_fail_res_e res_fail;
} alipay_union_code_pay_result_t;

/** Retained ABI; image decoding is unsupported. After initialization a valid
 * call returns 99, preserves order and records an UNSUPPORTED H2 diagnostic.
 * Invalid image/dimension/output arguments return ALIPAY_RV_WRONG_PARAM. */
retval_e alipay_union_code_parse_with_img(
    uint8_t* image, uint32_t width, uint32_t height,
    alipay_union_code_order_info_t* order);
retval_e alipay_union_code_parse_with_code(
    char* code, alipay_union_code_order_info_t* order);
retval_e alipay_union_code_confirm_payment(
    const alipay_union_code_order_info_t* order,
    alipay_union_code_pay_result_t* result);
retval_e alipay_nfc_url_parse(char* url, alipay_union_code_order_info_t* order);
retval_e alipay_nfc_parse_with_content(char* content,
                                       alipay_union_code_order_info_t* order);
retval_e alipay_nfc_confirm_payment(const alipay_union_code_order_info_t* order,
                                    alipay_union_code_pay_result_t* result);

typedef enum {
  BIZ_CMD_GET_PROFILE = 1,
  BIZ_CMD_SAVE_PROFILE,
  BIZ_CMD_LOGOUT,
  BIZ_CMD_GET_SDK_VERSION,
  BIZ_CMD_QUERY_BIND_STATUS,
  BIZ_CMD_GET_BIND_CODE,
  BIZ_CMD_SYNC_QUICK_PAY,
  BIZ_CMD_GET_NFC_PAYCODE,
  BIZ_CMD_UNKNOWN = 1000,
} alipay_biz_process_cmd_t;

retval_e alipay_biz_process_entry(const uint8_t* input, uint32_t input_length,
                                  uint8_t** output, uint32_t* output_length);
retval_e alipay_provision_ota(const char* appid);
retval_e alipay_active_device(const char* appid);
retval_e alipay_direct_provision(const char* data);
retval_e alipay_migrate_data(void);

typedef enum {
  DEPRECATED_RV_OK = 0,
} deprecated_retval_t;

typedef enum {
  DEPRECATED_STATUS_BINDING_FAIL = 0,
  DEPRECATED_STATUS_UNBINDED,
  DEPRECATED_STATUS_START_BINDING,
  DEPRECATED_STATUS_GETTING_PROFILE,
  DEPRECATED_STATUS_SAVING_DATA,
  DEPRECATED_STATUS_SAVING_DATA_OK,
  DEPRECATED_STATUS_FINISH_BINDING,
  DEPRECATED_STATUS_FINISH_BINDING_OK,
  DEPRECATED_STATUS_BINDING_OK = 0xa5,
  DEPRECATED_STATUS_UNKNOWN = 0xff,
} deprecated_binding_status_e;

#define ALIPAY_DEPRECATED_ITEM_PRIVATE_KEY (0x01 << 0)
#define ALIPAY_DEPRECATED_ITEM_SHARE_KEY (0x01 << 1)
#define ALIPAY_DEPRECATED_ITEM_SEED (0x01 << 2)
#define ALIPAY_DEPRECATED_ITEM_TIMEDIFF (0x01 << 3)
#define ALIPAY_DEPRECATED_ITEM_NICKNAME (0x01 << 4)
#define ALIPAY_DEPRECATED_ITEM_LOGONID (0x01 << 5)
#define ALIPAY_DEPRECATED_ITEM_BINDFLAG (0x01 << 6)
#define ALIPAY_DEPRECATED_ITEM_TID 65
#define ALIPAY_DEPRECATED_ITEM_UID 66
#define ALIPAY_DEPRECATED_ITEM_DEGRADE_STATUS 67
#define ALIPAY_DEPRECATED_ITEM_TRANSPORT_ACTIVITY_STATUS 68

int32_t ALIPAY_deprecated_storage_read(uint32_t item, uint8_t* content,
                                       uint32_t* length);
int32_t ALIPAY_deprecated_storage_write(uint32_t item, const uint8_t* content,
                                        uint32_t length);
int32_t ALIPAY_deprecated_storage_delete(uint32_t item);
int32_t ALIPAY_deprecated_storage_exists(uint32_t item);

typedef struct alipay_tansit_CardBaseVO {
  char cardNo[40];
  char cardType[40];
  char title[40];
} alipay_tansit_CardBaseVO_t;

typedef enum {
  TRANSIT_CARD_LIST_UNBIND = 0,
  TRANSIT_CARD_LIST_NET_ERROR = 1,
  TRANSIT_CARD_LIST_OWN_TOO_MANY_CARD = 2,
  TRANSIT_CARD_LIST_FUNC_NOT_SUPPPORT = 3,
  TRANSIT_CARD_LIST_DEV_ERROR = 4,
  TRANSIT_CARD_LIST_BUF_TOO_SHORT = 5,
  TRANSIT_CARD_LIST_SUCCESS = 99,
} transit_card_list_result_e;

typedef enum {
  TRANSIT_CODE_UNBIND = 0,
  TRANSIT_CODE_NET_ERROR = 1,
  TRANSIT_CODE_DATA_LIMIT = 2,
  TRANSIT_CODE_NOT_SUPPORT = 3,
  TRANSIT_CODE_SERVER_ERROR = 4,
  TRANSIT_CODE_DEV_ERROR = 5,
  TRANSIT_CODE_BALANCE_NOT_ENOUGH = 6,
  TRANSIT_CODE_ORDER_OVERDUE = 7,
  TRANSIT_CODE_REVOKING = 8,
  TRANSIT_CODE_SENIOR_CARD = 9,
  TRANSIT_CODE_MONTH_TICKET_REFUNDED = 10,
  TRANSIT_CODE_USER_INSURE_EXPIRE = 11,
  TRANSIT_CODE_GO_ANNUAL_REVIEW = 12,
  TRANSIT_CODE_GO_CHECK = 13,
  TRANSIT_CODE_COMMON_ERROR = 14,
  TRANSIT_BUY_DAY_CARD = 15,
  TRANSIT_CODE_NEED_RECHARGE = 16,
  TRANSIT_CODE_NOT_FOUND = 17,
  TRANSIT_CODE_SUCCESS = 99,
} transit_transit_code_result_e;

typedef struct {
  bool is_exists;
  int32_t expire_timestamp;
  int32_t remain_use_count;
} alipay_transit_card_status_t;

retval_e alipay_transit_get_activity_status_offline(bool* active);
retval_e alipay_transit_get_activity_status_online(bool* active);
retval_e alipay_transit_get_card_list_offline(alipay_tansit_CardBaseVO_t* cards,
                                              uint32_t* capacity,
                                              uint32_t* count);
retval_e alipay_transit_get_card_list_online(alipay_tansit_CardBaseVO_t* cards,
                                             uint32_t* capacity,
                                             uint32_t* count);
retval_e alipay_transit_update_card_data(char* card_no, char* card_type,
                                         char* error, uint32_t capacity);
retval_e alipay_transit_check_card_status(char* card_no, char* card_type,
                                          alipay_transit_card_status_t* status);
retval_e alipay_transit_get_TransitCode(char* card_no, char* card_type,
                                        uint8_t* code, uint32_t* length,
                                        char* error, uint32_t capacity);
retval_e alipay_transit_get_the_last_transitCode(char* title, uint8_t* code,
                                                 uint32_t* length, char* error,
                                                 uint32_t capacity);
retval_e alipay_transit_get_the_last_transcode_info(
    alipay_tansit_CardBaseVO_t* card);
int alipay_init_transit_module(void);
void alipay_break_rpc(void);
void alipay_get_last_card_list_result(transit_card_list_result_e* result);
void alipay_get_last_transit_code_result(transit_transit_code_result_e* result);

retval_e alipay_get_mac(uint8_t* output, uint32_t* length);
retval_e alipay_get_sn(uint8_t* output, uint32_t* length);
retval_e alipay_get_imei(uint8_t* output, uint32_t* length);
uint32_t alipay_get_productType(void);
retval_e alipay_get_companyName(uint8_t* output, uint32_t* length);
retval_e alipay_get_productModel(uint8_t* output, uint32_t* length);

#ifdef __cplusplus
}
#endif

#endif
