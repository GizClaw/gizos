#ifndef H2_ALIPAY_CLIENT_INTERNAL_H
#define H2_ALIPAY_CLIENT_INTERNAL_H

#include "h2_alipay_client.h"
#include "h2_crypto.h"

#define BINDING_TOKEN_MAX 128u
#define BINDING_ID_MAX 64u
#define BINDING_TEXT_MAX 256u
#define BINDING_SEED_MAX 128u
#define BINDING_BYTES_MAX                                                  \
  (49u + BINDING_TOKEN_MAX + 2u * BINDING_ID_MAX + 2u * BINDING_TEXT_MAX + \
   BINDING_SEED_MAX + 64u)
#define BINDING_STAGED 1u
#define BINDING_COMPLETE 2u
#define BINDING_MIGRATED 3u

typedef struct binding_record {
  uint8_t phase;
  char token[BINDING_TOKEN_MAX + 1u], tid[BINDING_ID_MAX + 1u],
      uid[BINDING_ID_MAX + 1u], nickname[BINDING_TEXT_MAX + 1u],
      login[BINDING_TEXT_MAX + 1u];
  uint8_t secret[32], seed[BINDING_SEED_MAX];
  size_t seed_length, nickname_length, login_length;
} binding_record_t;

static inline bool h2_alipay_client_binding_complete(
    const binding_record_t* record) {
  return record->phase == BINDING_COMPLETE || record->phase == BINDING_MIGRATED;
}

typedef struct client_state {
  uint8_t fingerprint[32];
  char device_name[132];
  char biztid[17];
} client_state_t;

typedef struct client_workspace {
  client_state_t state;
  h2_pal_p256_keypair_t keypair;
  uint8_t public_key[65];
  char canonical[8192];
  uint8_t response[8192];
} client_workspace_t;

int h2_alipay_client_load_binding(const h2_alipay_client_config_t* config,
                                  binding_record_t* record);
int h2_alipay_client_store_binding(const h2_alipay_client_config_t* config,
                                   const binding_record_t* record);
int h2_alipay_client_decode_factors(
    const h2_alipay_client_config_t* config, const char* encoded, size_t size,
    const h2_pal_p256_private_key_t* private_key, binding_record_t* record);
int h2_alipay_client_discard_binding(const h2_alipay_client_config_t* config);
int h2_alipay_client_migrate_data(
    const h2_alipay_client_config_t* config,
    h2_pal_p256_private_key_t* pending_private_key, bool* changed);
void h2_alipay_amount_bitmap(const char* text, char bitmap[32][14]);
int h2_alipay_paycode_iv(const char* mac, uint8_t out[16]);
int h2_alipay_paycode_generate(const h2_alipay_client_config_t* config,
                               const binding_record_t* binding, uint8_t* out,
                               size_t capacity, size_t* length,
                               h2_alipay_client_stage_t* stage);

void h2_alipay_client_wipe(void* data, size_t length);
void h2_alipay_client_fingerprint(const h2_alipay_client_config_t* config,
                                  uint8_t out[32]);
int h2_alipay_client_finish_namespace(h2_pal_pref_namespace_t* ns, int rc);
int h2_alipay_client_url_parameter(const char* url, const char* key, char* out,
                                   size_t capacity);
int h2_alipay_client_load_state(const h2_alipay_client_config_t* config,
                                client_state_t* state);
int h2_alipay_client_post(const h2_alipay_client_config_t* config,
                          client_workspace_t* workspace,
                          h2_alipay_client_result_t* result,
                          const char* operation, bool binary,
                          const uint8_t* body, size_t body_length,
                          size_t* response_length);
void* h2_alipay_client_json_alloc(void* user, size_t size);
void* h2_alipay_client_json_realloc(void* user, void* previous, size_t old_size,
                                    size_t size);
void h2_alipay_client_json_free(void* user, void* data);
struct yyjson_val;
struct yyjson_val* h2_alipay_client_json_member(struct yyjson_val* root,
                                                const char* name);
struct yyjson_mut_doc;
struct yyjson_mut_val;
int h2_alipay_client_profile_certificate(
    const h2_alipay_client_config_t* config, const char* factor,
    struct yyjson_mut_doc* document, struct yyjson_mut_val* output);
int h2_alipay_client_provision_receive(const h2_alipay_client_config_t* config,
                                       const char* input);
int h2_alipay_client_provision_request(const h2_alipay_client_config_t* config,
                                       const char* appid, bool activate,
                                       bool* activated,
                                       h2_alipay_client_payment_t* diagnostics);
#endif
