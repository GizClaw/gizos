#include "h2_haivivi_next_api.h"

#include <assert.h>

int main(void) {
  assert(H2_HAIVIVI_NEXT_OPERATION_COUNT == 172u);

  h2_haivivi_next_result_t (*auth_restrict)(
      h2_haivivi_next_client_t *,
      const h2_haivivi_next_legacyrestrict_token_post_auth_v1_sessions_restrict_token_request_t
          *,
      h2_haivivi_next_legacyrestrict_token_post_auth_v1_sessions_restrict_token_response_t
          *) =
      h2_haivivi_next_legacyrestrict_token_post_auth_v1_sessions_restrict_token;
  h2_haivivi_next_result_t (*push_restrict)(
      h2_haivivi_next_client_t *,
      const h2_haivivi_next_legacyrestrict_token_post_palapp_v1_sign_push_token_request_t
          *,
      h2_haivivi_next_legacyrestrict_token_post_palapp_v1_sign_push_token_response_t
          *) =
      h2_haivivi_next_legacyrestrict_token_post_palapp_v1_sign_push_token;
  assert(auth_restrict != NULL);
  assert(push_restrict != NULL);
  return 0;
}
