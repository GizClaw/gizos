#ifndef H2_LINGXI_H
#define H2_LINGXI_H

#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/os/h2_pal_json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Client storage limits, including string terminators. */
#define H2_LINGXI_ID_CAPACITY 65u
#define H2_LINGXI_DEVICE_NO_CAPACITY 65u
#define H2_LINGXI_SECRET_CAPACITY 257u
#define H2_LINGXI_PRODUCT_KEY_CAPACITY 513u
#define H2_LINGXI_BASE_URL_CAPACITY 513u
#define H2_LINGXI_IP_CAPACITY 65u
#define H2_LINGXI_TEXT_CAPACITY 257u
#define H2_LINGXI_MAX_INNER_IPS 16u

/** @brief The type already selected for this platform product. */
typedef enum h2_lingxi_device_no_type {
  H2_LINGXI_DEVICE_NO_SN = 1,
  H2_LINGXI_DEVICE_NO_IMEI = 2,
  H2_LINGXI_DEVICE_NO_CMEI = 3,
} h2_lingxi_device_no_type_t;

/** @brief Borrowed product credentials, valid for the synchronous call. */
typedef struct h2_lingxi_product {
  const char* id;
  const char* key;
} h2_lingxi_product_t;

/** @brief Caller-selected, already registered device identity. */
typedef struct h2_lingxi_identity {
  h2_lingxi_device_no_type_t type;
  const char* number;
} h2_lingxi_identity_t;

/**
 * @brief Caller-owned credential output; no pointer into a parser is retained.
 *
 * The owner persists these values if needed and clears them when no longer
 * used. The device number is supplied by the owner, never read from a modem.
 */
typedef struct h2_lingxi_device_info {
  char product_id[H2_LINGXI_ID_CAPACITY];
  char device_no[H2_LINGXI_DEVICE_NO_CAPACITY];
  char device_id[H2_LINGXI_ID_CAPACITY];
  char device_secret[H2_LINGXI_SECRET_CAPACITY];
} h2_lingxi_device_info_t;

/**
 * @brief Device heartbeat data borrowed for the call.
 *
 * The caller supplies actual observations and product policy. No metadata is
 * inferred, replaced or fabricated. inner_ips contains 1..MAX_INNER_IPS
 * nonempty strings. sdk_version begins with "ai_". The last three identity
 * fields may be NULL to encode an unavailable value as an empty string;
 * whether IMEI or CMEI is required depends on the actual product.
 */
typedef struct h2_lingxi_heartbeat {
  const char* const* inner_ips;
  size_t inner_ip_count;
  const char* net_speed;
  const char* net_type;
  const char* platform;
  const char* sdk_version;
  const char* firmware_version;
  const char* imei;
  const char* cmei;
  const char* mac;
} h2_lingxi_heartbeat_t;

/** @brief HTTP and platform outcomes, separate from the returned PAL result. */
typedef struct h2_lingxi_result {
  int http_status;
  int32_t platform_code;
  bool platform_success;
} h2_lingxi_result_t;

/**
 * @brief Borrowed dependencies and bounded request policy.
 *
 * Calls are synchronous. All dependencies and NUL-terminated input strings
 * remain valid until return. JSON provider access must be serialized by its
 * owner. Inputs must not alias outputs. The library owns only temporary
 * request/response storage and does not create a worker, discover identity,
 * persist credentials, schedule heartbeats or retry requests.
 *
 * base_url is an HTTPS origin, optionally ending in '/'. For an isolated
 * loopback fixture, allow_plaintext_loopback permits HTTP only to localhost,
 * 127.0.0.1 or [::1]. The HTTP provider must validate TLS and deliver response
 * headers before following redirects; a Location header aborts this request.
 *
 * json_limits bounds each encoded request and received response, plus JSON
 * parser/construction depth and values. All fields are explicit and obey PAL
 * limits. timeout_ms is the positive complete HTTP operation budget, no greater
 * than INT_MAX; JSON construction/parsing is additionally bounded by limits.
 */
typedef struct h2_lingxi_config {
  const h2_pal_mem_api_t* allocator;
  const h2_pal_http_api_t* http;
  const h2_pal_json_api_t* json;
  const char* base_url;
  /** Optional X-AI-IP; required by the platform for non-device callers. */
  const char* source_ip;
  const char* interface_name;
  h2_pal_json_limits_t json_limits;
  uint32_t timeout_ms;
  h2_pal_http_cancel_fn cancel;
  void* cancel_user;
  bool allow_plaintext_loopback;
} h2_lingxi_config_t;

/**
 * @brief Fetch the platform's existing credentials for a registered device.
 * @param config Borrowed call configuration.
 * @param product Borrowed product ID/key, bounded by the public capacities.
 * @param identity Borrowed device number and its registered type.
 * @param out_info Required output, cleared before validation and on failure.
 * @param out_result Required diagnostic output, cleared before validation.
 * @return OK only after HTTP 2xx, platform success/code 200, complete bounded
 *     credentials and exact returned product/device-number agreement. FORMAT
 *     rejects malformed responses; UNAVAILABLE reports HTTP/platform rejection.
 *
 * This calls the device information endpoint; it does not create a product,
 * import devices or change the registered device number type.
 */
h2_pal_result_t h2_lingxi_get_device_info(const h2_lingxi_config_t* config,
                                          const h2_lingxi_product_t* product,
                                          const h2_lingxi_identity_t* identity,
                                          h2_lingxi_device_info_t* out_info,
                                          h2_lingxi_result_t* out_result);

/**
 * @brief Send one heartbeat using caller-owned device credentials and data.
 * @param config Borrowed call configuration.
 * @param product Borrowed product ID/key.
 * @param device Borrowed credentials; product_id must match product.id.
 * @param heartbeat Borrowed actual metadata for this report.
 * @param out_result Required cleared diagnostic output.
 * @return OK only after HTTP 2xx and platform success/code 200. Transport,
 *     timeout, cancellation, JSON and provider errors propagate without retry.
 *
 * The owner decides when to send, how to persist successful state and how to
 * handle ambiguous delivery. API success alone is not platform UI acceptance.
 */
h2_pal_result_t h2_lingxi_report_heartbeat(
    const h2_lingxi_config_t* config, const h2_lingxi_product_t* product,
    const h2_lingxi_device_info_t* device,
    const h2_lingxi_heartbeat_t* heartbeat, h2_lingxi_result_t* out_result);

/** @brief Wipe caller-owned credentials. NULL is a no-op. */
void h2_lingxi_device_info_clear(h2_lingxi_device_info_t* info);

#ifdef __cplusplus
}
#endif
#endif
