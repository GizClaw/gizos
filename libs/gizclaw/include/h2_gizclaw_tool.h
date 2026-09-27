#ifndef H2_GIZCLAW_TOOL_H
#define H2_GIZCLAW_TOOL_H

#include "h2_gizclaw_rpc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Numeric procedure identity from the upstream ClientTool registry. */
typedef enum h2_gizclaw_tool {
  H2_GIZCLAW_TOOL_INFO_GET = 1,
  H2_GIZCLAW_TOOL_IDENTIFIERS_GET = 2,
  H2_GIZCLAW_TOOL_DEVICE_STATUS_GET = 3,
  H2_GIZCLAW_TOOL_DEVICE_REBOOT = 4,
  H2_GIZCLAW_TOOL_DEVICE_FACTORY_RESET = 5,
  H2_GIZCLAW_TOOL_DEVICE_FIND = 6,
  H2_GIZCLAW_TOOL_SOUND_PLAY = 7,
  H2_GIZCLAW_TOOL_WIFI_SCAN = 8,
  H2_GIZCLAW_TOOL_WIFI_CONNECT = 9,
  H2_GIZCLAW_TOOL_WIFI_SAVED_LIST = 10,
  H2_GIZCLAW_TOOL_WIFI_SAVED_FORGET = 11,
  H2_GIZCLAW_TOOL_FIRMWARE_UPDATE = 12,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_GET = 13,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_PLAY = 14,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_STOP = 15,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_MODE_SET = 16,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_PLAYLIST_GET = 17,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_PLAYLIST_SET = 18,
  H2_GIZCLAW_TOOL_AUDIOPLAYER_PLAYLIST_APPEND = 19,
  H2_GIZCLAW_TOOL_RUN_WORKSPACE_SET = 20,
  H2_GIZCLAW_TOOL_SOCIAL_PING = 21,
} h2_gizclaw_tool_t;

/** Invoke one tool on the RPC owner. Payloads are the tool's inner protobuf
 * messages; the SDK owns the tool/v0 envelope. Return a PAL result and fill
 * out_response, including a wire error for rejected arguments. Validate before
 * changing hardware. Views are borrowed until the response is consumed; never
 * return stack storage. Callbacks must return promptly and cannot stop or
 * destroy the Service. on_complete retains the response-completion contract
 * in h2_gizclaw_rpc.h. */
typedef int (*h2_gizclaw_tool_invoke_fn)(
    void *user, h2_gizclaw_tool_t tool, h2_gizclaw_rpc_bytes_t request_payload,
    h2_gizclaw_rpc_provider_response_t *out_response);

/** Immutable registration borrowed for the Client or Service lifetime. */
typedef struct h2_gizclaw_tool_handler {
  h2_gizclaw_tool_t tool;
  h2_gizclaw_tool_invoke_fn invoke;
  void *user;
} h2_gizclaw_tool_handler_t;

#ifdef __cplusplus
}
#endif
#endif
