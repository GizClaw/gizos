#ifndef H2_ANDROID_NET_H
#define H2_ANDROID_NET_H
#include "h2_android_platform.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Own one reference to the shared native POSIX/WolfSSL network provider.
 * The returned API is borrowed until destroy. Close every socket/resolver and
 * quiesce callers first. The owner does not change default AppHost assembly.
 * Default/required verification never falls back to insecure mode. */
typedef struct h2_android_net h2_android_net_t;
h2_pal_result_t h2_android_net_create(h2_android_net_t **out);
const h2_pal_net_api_t *h2_android_net_api(h2_android_net_t *owner);
/** Clears owner only after successful TLS reference release; NULL is allowed.
 */
h2_pal_result_t h2_android_net_destroy(h2_android_net_t **owner);
#ifdef __cplusplus
}
#endif
#endif
