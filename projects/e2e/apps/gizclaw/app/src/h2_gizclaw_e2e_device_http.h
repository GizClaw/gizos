#ifndef H2_GIZCLAW_E2E_DEVICE_HTTP_H
#define H2_GIZCLAW_E2E_DEVICE_HTTP_H
#include "h2_gizclaw_e2e_catalog.h"
#include "h2_gizclaw_api_key.h"
#include "h2_yyjson_json.h"

typedef struct h2_gizclaw_e2e_api {
  h2_gizclaw_e2e_fixture_t *fixture;
  h2_gizclaw_api_key_t key;
  h2_yyjson_json_t *provider;
  const h2_pal_json_api_t *json;
  h2_pal_json_document_t *document;
  h2_pal_json_value_t *root;
  uint32_t timeout_ms;
} h2_gizclaw_e2e_api_t;

int h2_gizclaw_e2e_api_call(h2_gizclaw_e2e_api_t *test, int method,
                            const char *path, const char *body, int expected);
int h2_gizclaw_e2e_tool_call(h2_gizclaw_e2e_api_t *test, const char *tool,
                             const char *args, int expected);
int h2_gizclaw_e2e_lua_upload(h2_gizclaw_e2e_api_t *test,
                              const uint8_t *archive, size_t size,
                              const char *sha256, int expected);
h2_pal_json_value_t *h2_gizclaw_e2e_api_field(h2_gizclaw_e2e_api_t *test,
                                              const char *path);
bool h2_gizclaw_e2e_api_text_is(h2_gizclaw_e2e_api_t *test, const char *path,
                                const char *expected);
bool h2_gizclaw_e2e_api_number_is(h2_gizclaw_e2e_api_t *test, const char *path,
                                  double expected);
#endif
