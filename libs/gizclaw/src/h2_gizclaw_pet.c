#include "h2_gizclaw_pet.h"
#include "h2_gizclaw_download_internal.h"
#include "h2_gizclaw_internal.h"
#include "h2_gizclaw_response_internal.h"
#include "h2_gizclaw_rpc.h"
#include "h2_gizclaw_service_internal.h"

#include "payload/gameplay.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include <limits.h>
#include <stdatomic.h>
#include <string.h>

typedef struct text_arg {
  const char *data;
  size_t len;
} text_arg_t;
typedef struct text_out {
  const h2_pal_mem_api_t *mem;
  char **out;
} text_out_t;
typedef struct pet_decode {
  const h2_pal_mem_api_t *mem;
  h2_gizclaw_pet_t *out;
  text_out_t text[7];
} pet_decode_t;

static bool encode_text(pb_ostream_t *s, const pb_field_t *f,
                        void *const *arg) {
  const text_arg_t *v = *arg;
  return v != NULL && (v->len == 0u || v->data != NULL) &&
         pb_encode_tag_for_field(s, f) &&
         pb_encode_string(s, (const pb_byte_t *)v->data, v->len);
}

static bool valid_utf8_span(const char *text, size_t len);

static bool decode_text(pb_istream_t *s, const pb_field_t *f, void **arg) {
  (void)f;
  text_out_t *v = *arg;
  if (v == NULL || v->mem == NULL || v->out == NULL || *v->out != NULL ||
      s->bytes_left > 4096u)
    return false;
  char *p = h2_pal_mem_alloc(v->mem, s->bytes_left + 1u);
  if (p == NULL)
    return false;
  const size_t n = s->bytes_left;
  if (!pb_read(s, (pb_byte_t *)p, n) || !valid_utf8_span(p, n)) {
    h2_pal_mem_free(v->mem, p);
    return false;
  }
  p[n] = '\0';
  *v->out = p;
  return true;
}

static void set_encoder(pb_callback_t *cb, text_arg_t *arg,
                        h2_gizclaw_str_t value) {
  *arg = (text_arg_t){value.data, value.len};
  cb->funcs.encode = encode_text;
  cb->arg = arg;
}

static void set_decoder(pb_callback_t *cb, text_out_t *arg,
                        const h2_pal_mem_api_t *mem, char **out) {
  *arg = (text_out_t){mem, out};
  cb->funcs.decode = decode_text;
  cb->arg = arg;
}

static void prepare_pet(gizclaw_rpc_v1_Pet *wire, pet_decode_t *ctx,
                        const h2_pal_mem_api_t *mem, h2_gizclaw_pet_t *out) {
  memset(out, 0, sizeof(*out));
  *ctx = (pet_decode_t){.mem = mem, .out = out};
  set_decoder(&wire->name, &ctx->text[0], mem, &out->name);
  set_decoder(&wire->pet_def_name, &ctx->text[1], mem, &out->pet_def_name);
  set_decoder(&wire->display_name, &ctx->text[2], mem, &out->display_name);
  set_decoder(&wire->workspace_name, &ctx->text[3], mem, &out->workspace_name);
  set_decoder(&wire->died_at, &ctx->text[4], mem, &out->died_at);
  set_decoder(&wire->state_settled_at, &ctx->text[5], mem,
              &out->state_settled_at);
  set_decoder(&wire->updated_at, &ctx->text[6], mem, &out->updated_at);
}

static void finish_pet(const gizclaw_rpc_v1_Pet *wire, h2_gizclaw_pet_t *out) {
  if (wire->has_stats)
    out->stats = (h2_gizclaw_pet_stats_t){
        wire->stats.life,    wire->stats.health, wire->stats.satiety,
        wire->stats.hygiene, wire->stats.mood,   wire->stats.energy};
  if (wire->has_progression) {
    out->experience = wire->progression.experience;
    out->level = wire->progression.level;
  }
  out->lifecycle = (h2_gizclaw_pet_lifecycle_t)wire->lifecycle;
}

static int encode_message(const h2_pal_mem_api_t *mem, const pb_msgdesc_t *desc,
                          const void *message, uint8_t **out, size_t *out_len) {
  *out = NULL;
  *out_len = 0u;
  pb_ostream_t sizing = PB_OSTREAM_SIZING;
  if (!pb_encode(&sizing, desc, message))
    return H2_PAL_ERR_FORMAT;
  uint8_t *p =
      h2_pal_mem_alloc(mem, sizing.bytes_written ? sizing.bytes_written : 1u);
  if (p == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  pb_ostream_t stream = pb_ostream_from_buffer(p, sizing.bytes_written);
  if (!pb_encode(&stream, desc, message)) {
    h2_pal_mem_free(mem, p);
    return H2_PAL_ERR_FORMAT;
  }
  *out = p;
  *out_len = stream.bytes_written;
  return H2_PAL_OK;
}

static void pet_value_clear(const h2_pal_mem_api_t *m, h2_gizclaw_pet_t *pet) {
  if (m == NULL || pet == NULL)
    return;
  h2_pal_mem_free(m, pet->name);
  h2_pal_mem_free(m, pet->pet_def_name);
  h2_pal_mem_free(m, pet->display_name);
  h2_pal_mem_free(m, pet->workspace_name);
  h2_pal_mem_free(m, pet->died_at);
  h2_pal_mem_free(m, pet->state_settled_at);
  h2_pal_mem_free(m, pet->updated_at);
  memset(pet, 0, sizeof(*pet));
}

static bool valid_optional_text(h2_gizclaw_str_t value) {
  return value.len <= 4096u && valid_utf8_span(value.data, value.len);
}

static bool valid_utf8_span(const char *text, size_t len) {
  if (text == NULL)
    return len == 0u;
  const unsigned char *cursor = (const unsigned char *)text;
  size_t remaining = len;
  while (remaining > 0u) {
    size_t width = 0u;
    if (cursor[0] == 0u) {
      return false;
    } else if (cursor[0] <= 0x7fu) {
      width = 1u;
    } else if (remaining >= 2u && cursor[0] >= 0xc2u && cursor[0] <= 0xdfu &&
               cursor[1] >= 0x80u && cursor[1] <= 0xbfu) {
      width = 2u;
    } else if (remaining >= 3u &&
               ((cursor[0] == 0xe0u && cursor[1] >= 0xa0u &&
                 cursor[1] <= 0xbfu) ||
                (cursor[0] >= 0xe1u && cursor[0] <= 0xecu &&
                 cursor[1] >= 0x80u && cursor[1] <= 0xbfu) ||
                (cursor[0] == 0xedu && cursor[1] >= 0x80u &&
                 cursor[1] <= 0x9fu) ||
                (cursor[0] >= 0xeeu && cursor[0] <= 0xefu &&
                 cursor[1] >= 0x80u && cursor[1] <= 0xbfu)) &&
               cursor[2] >= 0x80u && cursor[2] <= 0xbfu) {
      width = 3u;
    } else if (remaining >= 4u &&
               ((cursor[0] == 0xf0u && cursor[1] >= 0x90u &&
                 cursor[1] <= 0xbfu) ||
                (cursor[0] >= 0xf1u && cursor[0] <= 0xf3u &&
                 cursor[1] >= 0x80u && cursor[1] <= 0xbfu) ||
                (cursor[0] == 0xf4u && cursor[1] >= 0x80u &&
                 cursor[1] <= 0x8fu)) &&
               cursor[2] >= 0x80u && cursor[2] <= 0xbfu && cursor[3] >= 0x80u &&
               cursor[3] <= 0xbfu) {
      width = 4u;
    } else {
      return false;
    }
    cursor += width;
    remaining -= width;
  }
  return true;
}

static bool valid_game_result(const h2_gizclaw_pet_game_result_t *game_result) {
  return game_result != NULL && game_result->game_name.data != NULL &&
         game_result->game_name.len != 0u &&
         valid_optional_text(game_result->game_name) &&
         valid_optional_text(game_result->difficulty) &&
         valid_optional_text(game_result->outcome) &&
         valid_optional_text(game_result->occurred_at) &&
         (!game_result->has_duration_ms || game_result->duration_ms >= 0);
}

static void prepare_game_result(gizclaw_rpc_v1_PetDriveGameResultInput *wire,
                                const h2_gizclaw_pet_game_result_t *game_result,
                                h2_gizclaw_str_t idempotency_key,
                                text_arg_t text[5]) {
  set_encoder(&wire->game_name, &text[0], game_result->game_name);
  if (game_result->difficulty.len != 0u) {
    set_encoder(&wire->difficulty, &text[1], game_result->difficulty);
  }
  if (game_result->outcome.len != 0u) {
    set_encoder(&wire->outcome, &text[2], game_result->outcome);
  }
  if (game_result->occurred_at.len != 0u) {
    set_encoder(&wire->occurred_at, &text[3], game_result->occurred_at);
  }
  if (idempotency_key.len != 0u) {
    set_encoder(&wire->idempotency_key, &text[4], idempotency_key);
  }
  wire->has_score = game_result->has_score;
  wire->score = game_result->score;
  wire->has_max_score = game_result->has_max_score;
  wire->max_score = game_result->max_score;
  wire->has_duration_ms = game_result->has_duration_ms;
  wire->duration_ms = game_result->duration_ms;
}

typedef struct list_ctx {
  const h2_pal_mem_api_t *mem;
  h2_gizclaw_pet_page_t *page;
  size_t max;
  size_t capacity;
} list_ctx_t;
static bool decode_list_pet(pb_istream_t *s, const pb_field_t *f, void **arg) {
  (void)f;
  list_ctx_t *c = *arg;
  if (c->page->count >= c->max || c->page->count == SIZE_MAX ||
      c->page->count + 1u > SIZE_MAX / sizeof(c->page->items[0]))
    return false;
  if (c->page->count == c->capacity) {
    size_t capacity = c->capacity == 0u ? 4u : c->capacity * 2u;
    if (capacity < c->capacity || capacity > c->max)
      capacity = c->max;
    if (capacity > SIZE_MAX / sizeof(c->page->items[0]))
      return false;
    h2_gizclaw_pet_t *grown =
        h2_pal_mem_realloc(c->mem, c->page->items, capacity * sizeof(*grown));
    if (grown == NULL)
      return false;
    c->page->items = grown;
    c->capacity = capacity;
  }
  h2_gizclaw_pet_t *items = c->page->items;
  gizclaw_rpc_v1_Pet p = gizclaw_rpc_v1_Pet_init_zero;
  pet_decode_t pc;
  prepare_pet(&p, &pc, c->mem, &items[c->page->count]);
  if (!pb_decode(s, gizclaw_rpc_v1_Pet_fields, &p) ||
      items[c->page->count].name == NULL ||
      items[c->page->count].name[0] == '\0') {
    pet_value_clear(c->mem, &items[c->page->count]);
    return false;
  }
  finish_pet(&p, &items[c->page->count++]);
  return true;
}

typedef struct actions_clip_decode {
  const h2_pal_mem_api_t *mem;
  h2_gizclaw_pet_actions_t *actions;
} actions_clip_decode_t;

typedef struct actions_decode {
  text_out_t text[10];
  actions_clip_decode_t clips;
} actions_decode_t;

static bool decode_action_clip(pb_istream_t *stream, const pb_field_t *field,
                               void **arg) {
  (void)field;
  actions_clip_decode_t *context = *arg;
  if (context == NULL || context->mem == NULL || context->actions == NULL) {
    return false;
  }
  h2_gizclaw_pet_actions_t *actions = context->actions;
  if (actions->clip_name_count >= 256u)
    return false;
  const size_t count = actions->clip_name_count + 1u;
  const h2_pal_mem_api_t *mem = context->mem;
  h2_gizclaw_pet_clip_name_t *items =
      h2_pal_mem_realloc(mem, actions->clip_names, count * sizeof(*items));
  if (items == NULL) {
    return false;
  }
  actions->clip_names = items;
  h2_gizclaw_pet_clip_name_t *item = &items[actions->clip_name_count];
  memset(item, 0, sizeof(*item));
  gizclaw_rpc_v1_PetActions_ClipNamesEntry entry =
      gizclaw_rpc_v1_PetActions_ClipNamesEntry_init_zero;
  text_out_t text[2];
  set_decoder(&entry.key, &text[0], mem, &item->id);
  set_decoder(&entry.value, &text[1], mem, &item->pixa_clip_name);
  if (!pb_decode(stream, gizclaw_rpc_v1_PetActions_ClipNamesEntry_fields,
                 &entry) ||
      item->id == NULL || item->pixa_clip_name == NULL) {
    h2_pal_mem_free(mem, item->id);
    h2_pal_mem_free(mem, item->pixa_clip_name);
    memset(item, 0, sizeof(*item));
    return false;
  }
  actions->clip_name_count = count;
  return true;
}

static void prepare_actions(gizclaw_rpc_v1_PetActions *wire,
                            actions_decode_t *context,
                            const h2_pal_mem_api_t *mem,
                            h2_gizclaw_pet_actions_t *actions) {
  memset(actions, 0, sizeof(*actions));
  memset(context, 0, sizeof(*context));
  set_decoder(&wire->pet_name, &context->text[0], mem, &actions->pet_name);
  set_decoder(&wire->pet_def_name, &context->text[1], mem,
              &actions->pet_def_name);
  set_decoder(&wire->bindings.feed, &context->text[2], mem, &actions->feed);
  set_decoder(&wire->bindings.bathe, &context->text[3], mem, &actions->bathe);
  set_decoder(&wire->bindings.play, &context->text[4], mem, &actions->play);
  set_decoder(&wire->bindings.heal, &context->text[5], mem, &actions->heal);
  set_decoder(&wire->bindings.idle, &context->text[6], mem, &actions->idle);
  set_decoder(&wire->bindings.sick, &context->text[7], mem, &actions->sick);
  set_decoder(&wire->bindings.dead, &context->text[8], mem, &actions->dead);
  set_decoder(&wire->bindings.sleep, &context->text[9], mem, &actions->sleep);
  context->clips.mem = mem;
  context->clips.actions = actions;
  wire->clip_names.funcs.decode = decode_action_clip;
  wire->clip_names.arg = &context->clips;
}

static h2_pal_result_t
pet_create_message(h2_gizclaw_service_t *service, uint64_t identity,
                   const void *tag, h2_gizclaw_rpc_method_t method,
                   const pb_msgdesc_t *fields, const void *message,
                   uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *mem = service->config.client_config->allocator;
  uint8_t *payload = NULL;
  size_t len = 0u;
  h2_pal_result_t rc =
      (h2_pal_result_t)encode_message(mem, fields, message, &payload, &len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_rpc_internal(
        service, identity, method, tag, (h2_gizclaw_rpc_bytes_t){payload, len},
        timeout_ms, out_request);
  h2_pal_mem_free(mem, payload);
  return rc;
}

static const char pet_get_tag;
h2_pal_result_t h2_gizclaw_req_create_pet_get(h2_gizclaw_service_t *service,
                                              uint64_t identity,
                                              h2_gizclaw_str_t pet_name,
                                              uint32_t timeout_ms,
                                              h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_optional_text(pet_name) && pet_name.len > 0u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPetGetRequest message =
      gizclaw_rpc_v1_ServerPetGetRequest_init_zero;
  message.has_value = true;
  text_arg_t name_arg;
  set_encoder(&message.value.name, &name_arg, pet_name);
  return pet_create_message(service, identity, &pet_get_tag,
                            H2_GIZCLAW_RPC_SERVER_PET_GET,
                            gizclaw_rpc_v1_ServerPetGetRequest_fields, &message,
                            timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_pet_get(const h2_gizclaw_req_t *request,
                              h2_gizclaw_resp_storage_t *storage,
                              h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &pet_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  h2_gizclaw_pet_t result = {0};
  gizclaw_rpc_v1_ServerPetGetResponse decoded =
      gizclaw_rpc_v1_ServerPetGetResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  pet_decode_t context;
  prepare_pet(&decoded.value, &context, mem, &result);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetGetResponse_fields,
                 &decoded) ||
      !decoded.has_value || result.name == NULL || result.name[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  else
    finish_pet(&decoded.value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_pet_get(h2_gizclaw_service_t *service,
                                       h2_gizclaw_str_t pet_name,
                                       uint32_t timeout_ms,
                                       h2_gizclaw_resp_storage_t *storage,
                                       h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_get(service, 0u, pet_name,
                                                     timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char pet_delete_tag;
h2_pal_result_t h2_gizclaw_req_create_pet_delete(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_optional_text(pet_name) && pet_name.len > 0u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPetDeleteRequest message =
      gizclaw_rpc_v1_ServerPetDeleteRequest_init_zero;
  message.has_value = true;
  text_arg_t name_arg;
  set_encoder(&message.value.name, &name_arg, pet_name);
  return pet_create_message(service, identity, &pet_delete_tag,
                            H2_GIZCLAW_RPC_SERVER_PET_DELETE,
                            gizclaw_rpc_v1_ServerPetDeleteRequest_fields,
                            &message, timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_pet_delete(const h2_gizclaw_req_t *request,
                                 h2_gizclaw_resp_storage_t *storage,
                                 h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &pet_delete_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  h2_gizclaw_pet_t result = {0};
  gizclaw_rpc_v1_ServerPetDeleteResponse decoded =
      gizclaw_rpc_v1_ServerPetDeleteResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  pet_decode_t context;
  prepare_pet(&decoded.value, &context, mem, &result);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetDeleteResponse_fields,
                 &decoded) ||
      !decoded.has_value || result.name == NULL || result.name[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  else
    finish_pet(&decoded.value, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_pet_delete(h2_gizclaw_service_t *service,
                                          h2_gizclaw_str_t pet_name,
                                          uint32_t timeout_ms,
                                          h2_gizclaw_resp_storage_t *storage,
                                          h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_delete(service, 0u, pet_name,
                                                        timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_delete(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char pet_adopt_tag;
h2_pal_result_t h2_gizclaw_req_create_pet_adopt(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_adopt_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(options != NULL && options->name.len > 0u &&
        valid_optional_text(options->name) &&
        valid_optional_text(options->display_name)))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_RuntimeAdoptRequest message =
      gizclaw_rpc_v1_RuntimeAdoptRequest_init_zero;
  message.has_value = true;
  text_arg_t text[2];
  set_encoder(&message.value.name, &text[0], options->name);
  if (options->display_name.len > 0u)
    set_encoder(&message.value.display_name, &text[1], options->display_name);
  return pet_create_message(service, identity, &pet_adopt_tag,
                            H2_GIZCLAW_RPC_RUNTIME_ADOPT,
                            gizclaw_rpc_v1_RuntimeAdoptRequest_fields, &message,
                            timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_pet_adopt(const h2_gizclaw_req_t *request,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &pet_adopt_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  h2_gizclaw_pet_t result = {0};
  gizclaw_rpc_v1_RuntimeAdoptResponse decoded =
      gizclaw_rpc_v1_RuntimeAdoptResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  pet_decode_t context;
  prepare_pet(&decoded.value.pet, &context, mem, &result);
  if (!pb_decode(&stream, gizclaw_rpc_v1_RuntimeAdoptResponse_fields,
                 &decoded) ||
      !decoded.has_value || !decoded.value.has_pet || result.name == NULL ||
      result.name[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  else
    finish_pet(&decoded.value.pet, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_pet_adopt(
    h2_gizclaw_service_t *service,
    const h2_gizclaw_pet_adopt_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_adopt(service, 0u, options,
                                                       timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_adopt(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char pet_drive_tag;
h2_pal_result_t h2_gizclaw_req_create_pet_drive(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_drive_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(options != NULL && options->pet_name.len > 0u &&
        valid_optional_text(options->pet_name) &&
        valid_optional_text(options->idempotency_key) &&
        options->behavior >= H2_GIZCLAW_PET_BEHAVIOR_NONE &&
        options->behavior <= H2_GIZCLAW_PET_BEHAVIOR_HEAL &&
        !(options->behavior != H2_GIZCLAW_PET_BEHAVIOR_NONE &&
          options->game_result != NULL) &&
        (options->game_result == NULL ||
         valid_game_result(options->game_result))))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPetDriveRequest message =
      gizclaw_rpc_v1_ServerPetDriveRequest_init_zero;
  message.has_value = true;
  text_arg_t name_arg, key_arg, game_text[5];
  set_encoder(&message.value.pet_name, &name_arg, options->pet_name);
  if (options->behavior != H2_GIZCLAW_PET_BEHAVIOR_NONE) {
    message.value.has_behavior = true;
    message.value.behavior = (gizclaw_rpc_v1_PetBehavior)options->behavior;
  }
  if (options->game_result != NULL) {
    message.value.has_game_result = true;
    prepare_game_result(&message.value.game_result, options->game_result,
                        options->idempotency_key, game_text);
  } else if (options->idempotency_key.len > 0u)
    set_encoder(&message.value.idempotency_key, &key_arg,
                options->idempotency_key);
  return pet_create_message(service, identity, &pet_drive_tag,
                            H2_GIZCLAW_RPC_SERVER_PET_DRIVE,
                            gizclaw_rpc_v1_ServerPetDriveRequest_fields,
                            &message, timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_pet_drive(const h2_gizclaw_req_t *request,
                                h2_gizclaw_resp_storage_t *storage,
                                h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &pet_drive_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  h2_gizclaw_pet_t result = {0};
  gizclaw_rpc_v1_ServerPetDriveResponse decoded =
      gizclaw_rpc_v1_ServerPetDriveResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  pet_decode_t context;
  prepare_pet(&decoded.value.pet, &context, mem, &result);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetDriveResponse_fields,
                 &decoded) ||
      !decoded.has_value || !decoded.value.has_pet || result.name == NULL ||
      result.name[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  else
    finish_pet(&decoded.value.pet, &result);
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_pet_drive(
    h2_gizclaw_service_t *service,
    const h2_gizclaw_pet_drive_options_t *options, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage, h2_gizclaw_pet_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_drive(service, 0u, options,
                                                       timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_drive(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char pet_list_tag;
h2_pal_result_t h2_gizclaw_req_create_pet_list(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_optional_text(cursor) && cursor.len <= 255u && limit > 0u &&
        limit <= 64u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPetListRequest message =
      gizclaw_rpc_v1_ServerPetListRequest_init_zero;
  message.has_value = true;
  message.value.has_limit = true;
  message.value.limit = (int64_t)limit;
  text_arg_t cursor_arg;
  if (cursor.len > 0u)
    set_encoder(&message.value.cursor, &cursor_arg, cursor);
  return pet_create_message(service, identity, &pet_list_tag,
                            H2_GIZCLAW_RPC_SERVER_PET_LIST,
                            gizclaw_rpc_v1_ServerPetListRequest_fields,
                            &message, timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_pet_list(const h2_gizclaw_req_t *request,
                               h2_gizclaw_resp_storage_t *storage,
                               h2_gizclaw_pet_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &pet_list_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_rpc_bytes_t input;
  rc = h2_gizclaw_req_input_internal(request, &pet_list_tag, &input);
  if (rc != H2_PAL_OK)
    return rc;
  gizclaw_rpc_v1_ServerPetListRequest params =
      gizclaw_rpc_v1_ServerPetListRequest_init_zero;
  pb_istream_t input_stream = pb_istream_from_buffer(input.data, input.len);
  if (!pb_decode(&input_stream, gizclaw_rpc_v1_ServerPetListRequest_fields,
                 &params) ||
      !params.has_value || !params.value.has_limit || params.value.limit <= 0 ||
      params.value.limit > 64)
    return H2_PAL_ERR_FORMAT;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  h2_gizclaw_pet_page_t result = {0};
  gizclaw_rpc_v1_ServerPetListResponse decoded =
      gizclaw_rpc_v1_ServerPetListResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  list_ctx_t context = {
      .mem = mem, .page = &result, .max = (size_t)params.value.limit};
  text_out_t next;
  decoded.value.items.funcs.decode = decode_list_pet;
  decoded.value.items.arg = &context;
  set_decoder(&decoded.value.next_cursor, &next, mem, &result.next_cursor);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetListResponse_fields,
                 &decoded) ||
      !decoded.has_value ||
      (decoded.value.has_next &&
       (result.next_cursor == NULL || result.next_cursor[0] == '\0')))
    rc = H2_PAL_ERR_FORMAT;
  else
    result.has_next = decoded.value.has_next;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_pet_list(h2_gizclaw_service_t *service,
                                        h2_gizclaw_str_t cursor, size_t limit,
                                        uint32_t timeout_ms,
                                        h2_gizclaw_resp_storage_t *storage,
                                        h2_gizclaw_pet_page_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_list(
      service, 0u, cursor, limit, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_list(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

static const char pet_action_get_tag;
h2_pal_result_t h2_gizclaw_req_create_pet_action_get(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (!(valid_optional_text(pet_name) && pet_name.len > 0u))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPetActionsGetRequest message =
      gizclaw_rpc_v1_ServerPetActionsGetRequest_init_zero;
  message.has_value = true;
  text_arg_t name_arg;
  set_encoder(&message.value.name, &name_arg, pet_name);
  return pet_create_message(service, identity, &pet_action_get_tag,
                            H2_GIZCLAW_RPC_SERVER_PET_ACTIONS_GET,
                            gizclaw_rpc_v1_ServerPetActionsGetRequest_fields,
                            &message, timeout_ms, out_request);
}
h2_pal_result_t
h2_gizclaw_resp_parse_pet_action_get(const h2_gizclaw_req_t *request,
                                     h2_gizclaw_resp_storage_t *storage,
                                     h2_gizclaw_pet_actions_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const h2_gizclaw_rpc_response_t *response = NULL;
  h2_pal_result_t rc =
      h2_gizclaw_req_response_internal(request, &pet_action_get_tag, &response);
  if (rc != H2_PAL_OK)
    return rc;

  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  const h2_pal_mem_api_t *mem = &arena.allocator;
  h2_gizclaw_pet_actions_t result = {0};
  gizclaw_rpc_v1_ServerPetActionsGetResponse decoded =
      gizclaw_rpc_v1_ServerPetActionsGetResponse_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  actions_decode_t context;
  prepare_actions(&decoded.value, &context, mem, &result);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetActionsGetResponse_fields,
                 &decoded) ||
      !decoded.has_value || result.pet_name == NULL ||
      result.pet_name[0] == '\0')
    rc = H2_PAL_ERR_FORMAT;
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t
h2_gizclaw_rpc_pet_action_get(h2_gizclaw_service_t *service,
                              h2_gizclaw_str_t pet_name, uint32_t timeout_ms,
                              h2_gizclaw_resp_storage_t *storage,
                              h2_gizclaw_pet_actions_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_action_get(
      service, 0u, pet_name, timeout_ms, &request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, NULL, NULL, NULL, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait(request, H2_PAL_SYNC_WAIT_FOREVER);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_action_get(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}

typedef struct pixa_download {
  const h2_pal_mem_api_t *allocator;
  char *name;
  h2_gizclaw_pet_pixa_info_t info;
} pixa_download_t;

typedef struct pixa_sync_writer {
  h2_gizclaw_pet_pixa_write_fn write;
  void *user;
} pixa_sync_writer_t;

static h2_pal_result_t pixa_sync_output(void *opaque, const uint8_t *data,
                                        size_t len, size_t *out_written) {
  pixa_sync_writer_t *sink = opaque;
  h2_pal_result_t rc =
      (h2_pal_result_t)sink->write(sink->user, data, len);
  *out_written = rc == H2_PAL_OK ? len : 0u;
  return rc;
}

static void pixa_destroy(void *user) {
  pixa_download_t *context = user;
  h2_pal_mem_free(context->allocator, context->info.pet_name);
  h2_pal_mem_free(context->allocator, context->info.pet_def_name);
  h2_pal_mem_free(context->allocator, context->info.source_path);
  h2_pal_mem_free(context->allocator, context->name);
  h2_pal_mem_free(context->allocator, context);
}
static h2_pal_result_t pixa_metadata(void *user, h2_gizclaw_rpc_bytes_t payload,
                                     uint64_t *out_size) {
  pixa_download_t *context = user;
  gizclaw_rpc_v1_ServerPetPixaDownloadResponse decoded =
      gizclaw_rpc_v1_ServerPetPixaDownloadResponse_init_zero;
  text_out_t text[3];
  set_decoder(&decoded.value.pet_name, &text[0], context->allocator,
              &context->info.pet_name);
  set_decoder(&decoded.value.pet_def_name, &text[1], context->allocator,
              &context->info.pet_def_name);
  set_decoder(&decoded.value.pixa_path, &text[2], context->allocator,
              &context->info.source_path);
  pb_istream_t stream = pb_istream_from_buffer(payload.data, payload.len);
  if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetPixaDownloadResponse_fields,
                 &decoded) ||
      !decoded.has_value || decoded.value.size_bytes < 0 ||
      context->info.pet_name == NULL ||
      strcmp(context->info.pet_name, context->name) != 0 ||
      context->info.pet_def_name == NULL ||
      context->info.pet_def_name[0] == '\0' ||
      context->info.source_path == NULL || context->info.source_path[0] == '\0')
    return H2_PAL_ERR_FORMAT;
  context->info.size_bytes = (uint64_t)decoded.value.size_bytes;
  *out_size = context->info.size_bytes;
  return H2_PAL_OK;
}
static const h2_gizclaw_download_codec_t pixa_codec = {
    .metadata = pixa_metadata, .destroy = pixa_destroy};
static const char pet_pixa_download_tag;

h2_pal_result_t h2_gizclaw_req_create_pet_pixa_download(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    uint32_t timeout_ms, h2_gizclaw_req_t **out_request) {
  if (out_request != NULL)
    *out_request = NULL;
  if (service == NULL || out_request == NULL || pet_name.len == 0u ||
      !valid_optional_text(pet_name) ||
      timeout_ms == 0u || timeout_ms > INT32_MAX)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_mem_api_t *allocator = service->client_config.allocator;
  pixa_download_t *context = h2_pal_mem_alloc(allocator, sizeof(*context));
  if (context == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(context, 0, sizeof(*context));
  context->allocator = allocator;
  context->name = h2_pal_mem_alloc(allocator, pet_name.len + 1u);
  if (context->name == NULL) {
    pixa_destroy(context);
    return H2_PAL_ERR_NO_MEMORY;
  }
  memcpy(context->name, pet_name.data, pet_name.len);
  context->name[pet_name.len] = '\0';
  gizclaw_rpc_v1_ServerPetPixaDownloadRequest message =
      gizclaw_rpc_v1_ServerPetPixaDownloadRequest_init_zero;
  message.has_value = true;
  text_arg_t text;
  set_encoder(&message.value.pet_name, &text, pet_name);
  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      allocator, gizclaw_rpc_v1_ServerPetPixaDownloadRequest_fields, &message,
      &payload, &payload_len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_create_download_internal(
        service, identity, &pet_pixa_download_tag,
        H2_GIZCLAW_RPC_SERVER_PET_PIXA_DOWNLOAD,
        (h2_gizclaw_rpc_bytes_t){payload, payload_len}, timeout_ms, &pixa_codec,
        context, out_request);
  h2_pal_mem_free(allocator, payload);
  if (rc != H2_PAL_OK)
    pixa_destroy(context);
  return rc;
}
h2_pal_result_t h2_gizclaw_resp_parse_pet_pixa_download(
    const h2_gizclaw_req_t *request, h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_pet_pixa_info_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  const void *user = NULL;
  uint64_t received;
  h2_pal_result_t rc = h2_gizclaw_download_result_internal(
      request, &pet_pixa_download_tag, &user, &received);
  if (rc != H2_PAL_OK)
    return rc;
  const pixa_download_t *context = user;
  h2_gizclaw_resp_arena_t arena;
  rc = h2_gizclaw_resp_arena_begin(storage, &arena);
  if (rc != H2_PAL_OK)
    return rc;
  h2_gizclaw_pet_pixa_info_t result = {.size_bytes = context->info.size_bytes,
                                       .received_bytes = received};
  const char *source[] = {context->info.pet_name, context->info.pet_def_name,
                          context->info.source_path};
  char **destination[] = {&result.pet_name, &result.pet_def_name,
                          &result.source_path};
  for (size_t i = 0u; i < 3u; ++i) {
    size_t size = strlen(source[i]) + 1u;
    *destination[i] = h2_pal_mem_alloc(&arena.allocator, size);
    if (*destination[i] == NULL) {
      rc = H2_PAL_ERR_NO_MEMORY;
      break;
    }
    memcpy(*destination[i], source[i], size);
  }
  rc = h2_gizclaw_resp_arena_end(&arena, rc);
  if (rc == H2_PAL_OK)
    *out_result = result;
  return rc;
}
h2_pal_result_t h2_gizclaw_rpc_pet_pixa_download(
    h2_gizclaw_service_t *service, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_pixa_write_fn write, void *write_user, uint32_t timeout_ms,
    h2_gizclaw_resp_storage_t *storage,
    h2_gizclaw_pet_pixa_info_t *out_result) {
  if (out_result == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  if (storage == NULL || storage->used > storage->capacity ||
      (storage->capacity != 0u && storage->data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  /* The synchronous adapter calls the writer for every chunk; a missing
   * writer must fail here instead of when the first frame arrives. */
  if (write == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_req_t *request = NULL;
  h2_pal_result_t rc = h2_gizclaw_req_create_pet_pixa_download(
      service, 0u, pet_name, timeout_ms, &request);
  pixa_sync_writer_t writer = {.write = write, .user = write_user};
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_do(request, &writer, NULL, pixa_sync_output, NULL);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_req_wait_dispatch_internal(request);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_resp_parse_pet_pixa_download(request, storage, out_result);
  h2_gizclaw_req_release(request);
  return rc;
}
