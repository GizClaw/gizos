#include "h2_gizclaw_registration.h"

#include "h2_gizclaw_rpc.h"
#include "payload/system.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <string.h>

h2_pal_result_t
h2_gizclaw_client_register(h2_gizclaw_client_t *client, const char *token,
                           h2_gizclaw_registration_result_t *out_result) {
  if (out_result != NULL) {
    memset(out_result, 0, sizeof(*out_result));
  }
  if (client == NULL || token == NULL || token[0] == '\0' ||
      out_result == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const size_t token_len = strlen(token);
  if (token_len >= sizeof(((gizclaw_rpc_v1_ServerRegisterRequest *)0)->token)) {
    return H2_PAL_ERR_TRUNCATED;
  }

  gizclaw_rpc_v1_ServerRegisterRequest request =
      gizclaw_rpc_v1_ServerRegisterRequest_init_zero;
  memcpy(request.token, token, token_len + 1u);
  uint8_t request_bytes[gizclaw_rpc_v1_ServerRegisterRequest_size];
  pb_ostream_t output =
      pb_ostream_from_buffer(request_bytes, sizeof(request_bytes));
  if (!pb_encode(&output, gizclaw_rpc_v1_ServerRegisterRequest_fields,
                 &request)) {
    return H2_PAL_ERR_FORMAT;
  }

  h2_gizclaw_rpc_response_t response = {0};
  h2_pal_result_t result = h2_gizclaw_client_rpc_call(
      client, H2_GIZCLAW_RPC_SERVER_REGISTER,
      (h2_gizclaw_rpc_bytes_t){.data = request_bytes,
                               .len = output.bytes_written},
      &response);
  if (result == H2_PAL_OK && response.has_error) {
    result = H2_PAL_ERR_IO;
  }
  gizclaw_rpc_v1_ServerRegisterResponse decoded =
      gizclaw_rpc_v1_ServerRegisterResponse_init_zero;
  if (result == H2_PAL_OK) {
    pb_istream_t input = pb_istream_from_buffer(response.result_payload,
                                                response.result_payload_len);
    if (!pb_decode(&input, gizclaw_rpc_v1_ServerRegisterResponse_fields,
                   &decoded) ||
        decoded.runtime_profile_name[0] == '\0') {
      result = H2_PAL_ERR_FORMAT;
    }
  }
  if (result == H2_PAL_OK) {
    memcpy(out_result->runtime_profile_name, decoded.runtime_profile_name,
           sizeof(out_result->runtime_profile_name));
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return result;
}
