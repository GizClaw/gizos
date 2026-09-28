#ifndef H2_WEBRTC_FIXTURE_CLIENT_H
#define H2_WEBRTC_FIXTURE_CLIENT_H
#include "h2/pal/application/h2_pal_http.h"
#include "h2/pal/application/h2_pal_webrtc.h"
typedef struct h2_webrtc_fixture_client {
  const h2_pal_http_api_t *http;
  const char *offer_url;
  char session[64];
} h2_webrtc_fixture_client_t;
int h2_webrtc_fixture_exchange(void *user, h2_pal_webrtc_str_t offer,
                               char *answer, size_t capacity, size_t *out_len);
int h2_webrtc_fixture_close(void *user);
#endif
