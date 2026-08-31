#ifndef H2_GIZCLAW_RPC_H
#define H2_GIZCLAW_RPC_H

#include "h2_gizclaw_types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wire method number from gizclaw.rpc.v1.RpcMethod. */
typedef int32_t h2_gizclaw_rpc_method_t;

enum {
  H2_GIZCLAW_RPC_ALL_PING = 1,
  H2_GIZCLAW_RPC_CLIENT_INFO_GET = 3,
  H2_GIZCLAW_RPC_CLIENT_IDENTIFIERS_GET = 4,
  H2_GIZCLAW_RPC_SERVER_INFO_GET = 5,
  H2_GIZCLAW_RPC_SERVER_INFO_PUT = 6,
  H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_GET = 11,
  H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_SET = 12,
  H2_GIZCLAW_RPC_SERVER_RUN_WORKSPACE_RELOAD = 13,
  H2_GIZCLAW_RPC_SERVER_FIRMWARE_GET = 22,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_LIST = 24,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_GET = 25,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_CREATE = 26,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_PUT = 27,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_DELETE = 28,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_LIST = 29,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_GET = 30,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_AUDIO_DOWNLOAD = 31,
  H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_AUDIO_GET =
      H2_GIZCLAW_RPC_SERVER_WORKSPACE_HISTORY_AUDIO_DOWNLOAD,
  H2_GIZCLAW_RPC_SERVER_WORKFLOW_LIST = 32,
  H2_GIZCLAW_RPC_SERVER_WORKFLOW_GET = 33,
  H2_GIZCLAW_RPC_SERVER_CONTACT_LIST = 38,
  H2_GIZCLAW_RPC_SERVER_CONTACT_GET = 39,
  H2_GIZCLAW_RPC_SERVER_CONTACT_CREATE = 40,
  H2_GIZCLAW_RPC_SERVER_CONTACT_PUT = 41,
  H2_GIZCLAW_RPC_SERVER_CONTACT_DELETE = 42,
  H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_GET = 43,
  H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_CREATE = 44,
  H2_GIZCLAW_RPC_SERVER_FRIEND_INVITE_TOKEN_CLEAR = 45,
  H2_GIZCLAW_RPC_SERVER_FRIEND_ADD = 46,
  H2_GIZCLAW_RPC_SERVER_FRIEND_LIST = 47,
  H2_GIZCLAW_RPC_SERVER_FRIEND_DELETE = 48,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_LIST = 49,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_GET = 50,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_CREATE = 51,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_PUT = 52,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_DELETE = 53,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_GET = 54,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_CREATE = 55,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_INVITE_TOKEN_CLEAR = 56,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_JOIN = 57,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_LIST = 58,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_ADD = 59,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_PUT = 60,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MEMBERS_DELETE = 61,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_LIST = 62,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_GET = 63,
  H2_GIZCLAW_RPC_SERVER_PET_LIST = 65,
  H2_GIZCLAW_RPC_SERVER_PET_GET = 66,
  H2_GIZCLAW_RPC_RUNTIME_ADOPT = 67,
  H2_GIZCLAW_RPC_SERVER_PET_PUT = 68,
  H2_GIZCLAW_RPC_SERVER_PET_DELETE = 69,
  H2_GIZCLAW_RPC_SERVER_PET_DRIVE = 70,
  H2_GIZCLAW_RPC_SERVER_POINTS_GET = 71,
  H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_LIST = 72,
  H2_GIZCLAW_RPC_SERVER_POINTS_TRANSACTIONS_GET = 73,
  H2_GIZCLAW_RPC_CLIENT_TOOL_INVOKE = 82,
  H2_GIZCLAW_RPC_SERVER_PET_ACTIONS_GET = 86,
  H2_GIZCLAW_RPC_SERVER_PET_PIXA_DOWNLOAD = 87,
  H2_GIZCLAW_RPC_SERVER_FRIEND_INFO_GET = 89,
  H2_GIZCLAW_RPC_SERVER_REGISTER = 90,
  H2_GIZCLAW_RPC_SERVER_SPEECH_TRANSCRIBE = 91,
  H2_GIZCLAW_RPC_SERVER_SPEECH_SYNTHESIZE = 92,
  H2_GIZCLAW_RPC_SERVER_PEER_DELETE = 93,
  H2_GIZCLAW_RPC_SERVER_SPEECH_EXTRACT = 94,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_DOWNLOAD = 95,
  H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_GET =
      H2_GIZCLAW_RPC_SERVER_FRIEND_GROUP_MESSAGES_AUDIO_DOWNLOAD,
};

enum {
  H2_GIZCLAW_RPC_ERROR_PARSE = -32700,
  H2_GIZCLAW_RPC_ERROR_INVALID_REQUEST = -32600,
  H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND = -32601,
  H2_GIZCLAW_RPC_ERROR_INVALID_PARAMS = -32602,
  H2_GIZCLAW_RPC_ERROR_INTERNAL = -32603,
  H2_GIZCLAW_RPC_ERROR_BAD_REQUEST = 400,
  H2_GIZCLAW_RPC_ERROR_FORBIDDEN = 403,
  H2_GIZCLAW_RPC_ERROR_NOT_FOUND = 404,
  H2_GIZCLAW_RPC_ERROR_CONFLICT = 409,
};

typedef struct h2_gizclaw_rpc_bytes {
  const uint8_t *data;
  size_t len;
} h2_gizclaw_rpc_bytes_t;

/** Owned unary result. Release it with h2_gizclaw_rpc_response_deinit(). */
typedef struct h2_gizclaw_rpc_response {
  uint8_t *result_payload;
  size_t result_payload_len;
  bool has_error;
  int error_code;
  char *error_message;
  size_t error_message_len;
} h2_gizclaw_rpc_response_t;

typedef struct h2_gizclaw_rpc_request h2_gizclaw_rpc_request_t;

typedef enum h2_gizclaw_rpc_stream_event_kind {
  H2_GIZCLAW_RPC_STREAM_RESPONSE = 1,
  H2_GIZCLAW_RPC_STREAM_DATA,
  H2_GIZCLAW_RPC_STREAM_EOS,
} h2_gizclaw_rpc_stream_event_kind_t;

/** Views are borrowed and valid only for the duration of the callback. */
typedef struct h2_gizclaw_rpc_stream_event {
  h2_gizclaw_rpc_stream_event_kind_t kind;
  h2_gizclaw_rpc_bytes_t result_payload;
  h2_gizclaw_rpc_bytes_t data;
  bool has_error;
  int error_code;
  h2_gizclaw_rpc_bytes_t error_message;
} h2_gizclaw_rpc_stream_event_t;

typedef int (*h2_gizclaw_rpc_stream_fn)(
    void *user, const h2_gizclaw_rpc_stream_event_t *event);

typedef struct h2_gizclaw_rpc_provider_response {
  h2_gizclaw_rpc_bytes_t payload;
  bool has_error;
  int error_code;
  h2_gizclaw_rpc_bytes_t error_message;
} h2_gizclaw_rpc_provider_response_t;

/**
 * Handle a server-initiated client.* method.
 *
 * Request and response payloads are protobuf message bytes. Returned views are
 * borrowed and need to remain valid only until the callback returns.
 */
typedef int (*h2_gizclaw_rpc_provider_fn)(
    void *user, h2_gizclaw_rpc_method_t method,
    h2_gizclaw_rpc_bytes_t request_payload,
    h2_gizclaw_rpc_provider_response_t *out_response);

/** Call any unary GizClaw RPC with an already protobuf-encoded payload. */
int h2_gizclaw_client_rpc_call(h2_gizclaw_client_t *client,
                               h2_gizclaw_rpc_method_t method,
                               h2_gizclaw_rpc_bytes_t params_payload,
                               h2_gizclaw_rpc_response_t *out_response);

/**
 * Start one request-owned unary RPC on the connected client's Peer service.
 *
 * Calls that start requests, poll the client, inspect results, cancel, and
 * destroy requests must be serialized by one caller. The client and its PAL
 * configuration must outlive every request. The payload is borrowed only
 * until this function returns.
 */
int h2_gizclaw_client_rpc_request_start(h2_gizclaw_client_t *client,
                                        h2_gizclaw_rpc_method_t method,
                                        h2_gizclaw_rpc_bytes_t params_payload,
                                        uint32_t timeout_ms,
                                        h2_gizclaw_rpc_request_t **out_request);

/**
 * Inspect one request without polling.
 *
 * Returns H2_PAL_ERR_WOULD_BLOCK while pending. A successful response is an
 * owned copy and must be released with h2_gizclaw_rpc_response_deinit(). A
 * remote RPC error remains a successful transport result with has_error set.
 */
int h2_gizclaw_rpc_request_result(h2_gizclaw_rpc_request_t *request,
                                  h2_gizclaw_rpc_response_t *out_response);

/** Idempotently cancel a pending request. */
void h2_gizclaw_rpc_request_cancel(h2_gizclaw_rpc_request_t *request);

/** Cancel if needed and consume the request handle. NULL is a no-op. */
void h2_gizclaw_rpc_request_destroy(h2_gizclaw_rpc_request_t *request);

/**
 * Call any server-streaming GizClaw RPC.
 *
 * The callback receives exactly one RESPONSE, zero or more DATA events, then
 * one EOS event on success. Returning a non-zero value cancels the call.
 */
int h2_gizclaw_client_rpc_call_stream(h2_gizclaw_client_t *client,
                                      h2_gizclaw_rpc_method_t method,
                                      h2_gizclaw_rpc_bytes_t params_payload,
                                      h2_gizclaw_rpc_stream_fn on_event,
                                      void *user);

void h2_gizclaw_rpc_response_deinit(h2_gizclaw_client_t *client,
                                    h2_gizclaw_rpc_response_t *response);

#ifdef __cplusplus
}
#endif

#endif
