#include "h2_loader_digest.h"

#include <string.h>

static int digest_start(void* user) {
  h2_loader_sha256_digest_t* state = user;
  if (state == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(state, 0, sizeof(*state));
  h2_loader_sha256_init(&state->sha);
  state->active = 1;
  return H2_PAL_OK;
}

static int digest_update(void* user, const uint8_t* data, size_t len) {
  h2_loader_sha256_digest_t* state = user;
  if (state == NULL || (data == NULL && len != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!state->active) return H2_PAL_ERR_INVALID_STATE;
  h2_loader_sha256_update(&state->sha, data, len);
  return H2_PAL_OK;
}

static int digest_finish(void* user, uint8_t out_digest[32]) {
  h2_loader_sha256_digest_t* state = user;
  if (out_digest == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_digest, 0, 32u);
  if (state == NULL) return H2_PAL_ERR_INVALID_ARG;
  if (!state->active) return H2_PAL_ERR_INVALID_STATE;
  h2_loader_sha256_finish(&state->sha, out_digest);
  memset(state, 0, sizeof(*state));
  return H2_PAL_OK;
}

static void digest_abort(void* user) {
  h2_loader_sha256_digest_t* state = user;
  if (state != NULL) memset(state, 0, sizeof(*state));
}

h2_pal_result_t h2_loader_sha256_digest_init(h2_loader_sha256_digest_t* state,
                                             h2_loader_digest_api_t* out_api) {
  if (out_api == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_api = (h2_loader_digest_api_t){0};
  if (state == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(state, 0, sizeof(*state));
  *out_api = (h2_loader_digest_api_t){
      .user = state,
      .start = digest_start,
      .update = digest_update,
      .finish = digest_finish,
      .abort = digest_abort,
  };
  return H2_PAL_OK;
}
