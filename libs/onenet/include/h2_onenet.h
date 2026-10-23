#ifndef H2_ONENET_H
#define H2_ONENET_H

#include "h2/pal/application/h2_pal_mqtt.h"
#include "h2/pal/os/h2_pal_crypto.h"
#include "h2/pal/os/h2_pal_json.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Storage limits of this client, including string terminators. */
#define H2_ONENET_NAME_CAPACITY 65u
#define H2_ONENET_HOST_CAPACITY 256u
#define H2_ONENET_CLIENT_ID_CAPACITY 512u
#define H2_ONENET_TOKEN_CAPACITY 1024u
#define H2_ONENET_REQUEST_ID_CAPACITY 14u
#define H2_ONENET_KEY_MAX_BYTES 128u
#define H2_ONENET_MAX_REQUEST_ID UINT64_C(9999999999999)
/** @brief Minimum budgets needed for a OneJSON envelope and its params. */
#define H2_ONENET_MIN_PAYLOAD_BYTES 64u
#define H2_ONENET_MIN_JSON_DEPTH 2u
#define H2_ONENET_MIN_JSON_VALUES 8u

typedef struct h2_onenet h2_onenet_t;

/** @brief Connection metadata supplied by the device owner, never discovered.
 */
typedef struct h2_onenet_identity {
  /** 0: non-cellular; 1: China Mobile; 2: Telecom; 3: Unicom; 4: Broadnet. */
  uint8_t network;
  const char* imei;   /**< Cellular: required 15 decimal digits. */
  const char* iccid;  /**< Optional 20 characters beginning with 8986. */
  const char* imsi;   /**< Optional 15 decimal digits beginning with 460. */
  const char* msisdn; /**< Optional 11 or 13 decimal digits. */
  const char* sn;     /**< Non-cellular: SN or MAC, 10..32 alphanumeric/-/:. */
  const char* mac;    /**< Non-cellular: SN or MAC, 10..32 alphanumeric/-/:. */
} h2_onenet_identity_t;

/**
 * @brief Build a OneNET CONNECT client ID with optional fusion metadata.
 * @param device_name Borrowed NUL-terminated topic segment, valid for the call.
 *     Names are nonempty ASCII letters/digits or _ - . : @, shorter than
 *     H2_ONENET_NAME_CAPACITY. This same rule applies to product and service
 *     topic segments.
 * @param identity Borrowed metadata, or NULL for the plain device name.
 *     Cellular connections require at least one of ICCID, IMSI and MSISDN.
 * @param out Caller-owned storage, cleared on failure when capacity is nonzero.
 * @param capacity Storage bytes, including the NUL terminator.
 * @return PAL result; NO_SPACE means the output does not fit.
 */
h2_pal_result_t h2_onenet_build_client_id(const char* device_name,
                                          const h2_onenet_identity_t* identity,
                                          char* out, size_t capacity);

/**
 * @brief Build a device-scoped OneNET 2018-10-31 HMAC-SHA1 access token.
 * @param crypto Borrowed Crypto PAL providing hmac_sha1.
 * @param product_id Borrowed product topic segment, valid for the call.
 * @param device_name Borrowed device topic segment, valid for the call.
 * @param key_base64 Borrowed canonical padded Base64 device/product key.
 *     Decoded keys are limited by H2_ONENET_KEY_MAX_BYTES.
 * @param expires_at_unix_s Absolute expiry in Unix seconds, nonzero. The caller
 *     supplies calibrated time and renews the token before expiry.
 * @param out Caller-owned text storage, cleared on failure.
 * @param capacity Storage bytes, including the terminator.
 * @return PAL result; provider failures propagate without truncation.
 *
 * No secret is retained, logged or persisted. Temporary decoded key material
 * is wiped. To use another platform-supported signing method, the caller may
 * supply its own token to h2_onenet_connect().
 */
h2_pal_result_t h2_onenet_token_generate(const h2_pal_crypto_api_t* crypto,
                                         const char* product_id,
                                         const char* device_name,
                                         const char* key_base64,
                                         uint64_t expires_at_unix_s, char* out,
                                         size_t capacity);

/** @brief Direct-device OneJSON operations; gateway/legacy/OTA are separate. */
typedef enum h2_onenet_operation {
  H2_ONENET_PROPERTY_POST = 1,
  H2_ONENET_EVENT_POST = 2,
  H2_ONENET_DESIRED_GET = 3,
  H2_ONENET_DESIRED_DELETE = 4,
  H2_ONENET_PROPERTY_SET = 5,
  H2_ONENET_PROPERTY_GET = 6,
  H2_ONENET_SERVICE_INVOKE = 7,
} h2_onenet_operation_t;

typedef enum h2_onenet_event_type {
  H2_ONENET_CONNECTED = 1, /**< CONNECT and SUBACK both succeeded. */
  H2_ONENET_DISCONNECTED = 2,
  H2_ONENET_RESPONSE = 3, /**< Platform response, timeout or cancellation. */
  H2_ONENET_COMMAND = 4,  /**< Incoming command; reply using command_handle. */
  H2_ONENET_COMMAND_EXPIRED = 5,
  H2_ONENET_ERROR = 6,
} h2_onenet_event_type_t;

/**
 * @brief Synchronously borrowed event, valid only during the callback.
 *
 * JSON data uses the injected JSON PAL and preserves JSON value types. Copy
 * application data before returning if it is needed asynchronously. Only the
 * integer command_handle may be retained for a later h2_onenet_reply().
 */
typedef struct h2_onenet_event {
  h2_onenet_event_type_t type;
  h2_onenet_operation_t operation;
  h2_pal_result_t
      result; /**< Local result; code is the separate platform code. */
  int32_t code;
  const char* request_id;
  const char* service;
  const h2_pal_json_value_t* data;
  uint64_t command_handle;
} h2_onenet_event_t;

/**
 * @brief Application event callback, executed outside the MQTT callback.
 *
 * All client operations run on one owner task. From this callback only
 * h2_onenet_reply() and h2_onenet_is_connected() may be called. Recursive
 * process/connect/publish/disconnect/destroy return BUSY.
 */
typedef void (*h2_onenet_event_fn)(void* user, h2_onenet_t* client,
                                   const h2_onenet_event_t* event);

/**
 * @brief Explicit instance budgets, allocated once during create.
 *
 * payload_bytes bounds every received and transmitted OneJSON document.
 * It must meet MIN_PAYLOAD_BYTES; json_depth and json_values must meet
 * MIN_JSON_DEPTH and MIN_JSON_VALUES. All remaining budgets are nonzero.
 * JSON parser allocations additionally obey json_depth and json_values.
 * pending_requests bounds each of the outbound and inbound request tables.
 * queued_messages bounds receive buffering per process call; overflow returns
 * NO_SPACE and closes the connection rather than silently accepting data loss.
 */
typedef struct h2_onenet_limits {
  size_t payload_bytes;
  size_t json_depth;
  size_t json_values;
  size_t pending_requests;
  size_t queued_messages;
} h2_onenet_limits_t;

/**
 * @brief Caller-selected endpoint, dependencies, timing and resource budgets.
 *
 * PAL objects and their backends, TLS config, bind config, and event user are
 * borrowed until destroy. Names, host and identity are copied during create.
 * Each instance owns its MQTT client, buffers and request tables. No worker,
 * provider, network interface, credential store or product policy is owned.
 * The MQTT provider must invoke callbacks synchronously from its operations.
 */
typedef struct h2_onenet_config {
  const h2_pal_mem_api_t* allocator;
  const h2_pal_mqtt_api_t* mqtt;
  const h2_pal_json_api_t* json;
  const h2_pal_time_api_t* time;
  const char* product_id;
  const char* device_name;
  const h2_onenet_identity_t* identity;
  h2_pal_mqtt_endpoint_t endpoint;
  h2_pal_mqtt_transport_t transport;
  const h2_pal_net_tls_config_t* tls;
  const h2_pal_net_bind_t* bind;
  h2_onenet_limits_t limits;
  size_t mqtt_buffer_bytes;
  uint16_t keepalive_sec;
  uint32_t operation_timeout_ms;
  uint32_t response_timeout_ms;
  /** First outbound ID, 1..MAX_REQUEST_ID. The owner chooses a non-reused seed
   * across recreated instances when platform jobs may survive a connection. */
  uint64_t initial_request_id;
  h2_onenet_event_fn on_event;
  void* event_user;
} h2_onenet_config_t;

/**
 * @brief Create a disconnected client, clearing out_client before validation.
 * @param config Borrowed configuration for the call; retention is described
 *     in h2_onenet_config_t.
 * @param out_client Required owned output; NULL on every failure.
 * @return PAL result; allocation or size overflow leaves no owned resources.
 */
h2_pal_result_t h2_onenet_create(const h2_onenet_config_t* config,
                                 h2_onenet_t** out_client);

/**
 * @brief Connect or reconnect with a copied, nonempty access token.
 * @param client Owned client, used by its sole owner task.
 * @param token Borrowed token for the call; bounded by TOKEN_CAPACITY.
 * @return PAL result. Success starts subscription setup; process must receive
 *     SUBACK before publish/reply are available or CONNECTED is emitted.
 *
 * A reconnect closes the prior transport and cancels outstanding requests.
 * Caller policy chooses reconnect backoff and token renewal. All MQTT calls
 * have the configured operation budget; no background retries are started.
 */
h2_pal_result_t h2_onenet_connect(h2_onenet_t* client, const char* token);

/**
 * @brief Pump MQTT, dispatch copied incoming messages, and expire requests.
 * @param client Sole-owner client.
 * @param timeout_ms MQTT receive/poll budget; zero is a poll. Pending response
 *     deadlines are independent and checked after the call.
 * @return PAL result. A normal MQTT receive timeout/would-block is successful
 *     polling. Malformed OneJSON returns FORMAT and emits ERROR. Transport
 *     failure or receive overflow cancels requests and closes the connection.
 */
h2_pal_result_t h2_onenet_process(h2_onenet_t* client, uint32_t timeout_ms);

/**
 * @brief Publish one direct-device OneJSON request with a tracked response.
 * @param client Ready client, outside an event callback.
 * @param operation PROPERTY_POST, EVENT_POST, DESIRED_GET or DESIRED_DELETE.
 * @param params Borrowed complete JSON params bytes for this call. Post uses
 *     an object; desired operations use an array of property identifiers.
 * @param params_len Byte length; parsing obeys the instance JSON limits.
 * @param out_request_id Required output, cleared on failure.
 * @return PAL result. FULL means no request slot is available. MQTT acceptance
 *     is not platform acceptance: completion arrives as RESPONSE, matching
 *     both the operation and ID. IDs are never reused within the instance.
 */
h2_pal_result_t h2_onenet_publish(h2_onenet_t* client,
                                  h2_onenet_operation_t operation,
                                  const uint8_t* params, size_t params_len,
                                  uint64_t* out_request_id);

/**
 * @brief Complete a pending incoming command, synchronously or asynchronously.
 * @param client Ready client on its owner task; allowed in an event callback.
 * @param command_handle Handle from COMMAND, valid until reply, expiry or
 *     disconnect. Handles are never reused within an instance.
 * @param code Nonnegative application/platform result code.
 * @param data Borrowed complete JSON object for the reply data, or NULL/0 to
 *     omit data. Properties get and service consumers supply their results.
 * @param data_len JSON byte length.
 * @return PAL result. NOT_FOUND rejects stale/already completed handles.
 *     A failed send retains the command for retry until its deadline.
 *
 * Duplicate commands with the same pending topic and ID are coalesced. After
 * completion the application remains responsible for business idempotency.
 */
h2_pal_result_t h2_onenet_reply(h2_onenet_t* client, uint64_t command_handle,
                                int32_t code, const uint8_t* data,
                                size_t data_len);

/** @brief True only after successful connection and subscription setup. */
bool h2_onenet_is_connected(const h2_onenet_t* client);

/**
 * @brief Disconnect, close owned transport, and cancel outstanding requests.
 * @param client Sole-owner client; disconnected calls are successful no-ops.
 * @return PAL result. Cleanup occurs even if the MQTT DISCONNECT send fails.
 */
h2_pal_result_t h2_onenet_disconnect(h2_onenet_t* client);

/**
 * @brief Destroy without callbacks, close transport, and wipe the copied token.
 * @param client Required pointer to the owned handle. NULL handle is a
 *     successful no-op. BUSY leaves the handle valid and unchanged.
 * @return PAL result; success clears the handle. Borrowed providers survive.
 */
h2_pal_result_t h2_onenet_destroy(h2_onenet_t** client);

#ifdef __cplusplus
}
#endif
#endif
