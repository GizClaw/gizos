#ifndef H2_LOADER_DIGEST_H
#define H2_LOADER_DIGEST_H

#include "h2_loader_package.h"
#include "h2_loader_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned portable SHA-256 callback state. Serialize users of one state.
 */
typedef struct h2_loader_sha256_digest {
  h2_loader_sha256_t sha;
  int active;
} h2_loader_sha256_digest_t;

/** Reset the state and bind borrowed callbacks for start/update/finish/abort.
 * start may restart an unfinished digest. finish and abort clear its state.
 * The caller owns both objects for the full callback lifetime; digest output
 * must not overlap the state. Scheduling belongs to the caller, not the hash.
 */
h2_pal_result_t h2_loader_sha256_digest_init(h2_loader_sha256_digest_t* state,
                                             h2_loader_digest_api_t* out_api);

#ifdef __cplusplus
}
#endif

#endif
