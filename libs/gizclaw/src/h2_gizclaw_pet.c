#include "h2_gizclaw_pet.h"
#include "h2_gizclaw_internal.h"
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

static bool decode_text(pb_istream_t *s, const pb_field_t *f, void **arg) {
  (void)f;
  text_out_t *v = *arg;
  if (v == NULL || v->mem == NULL || v->out == NULL || *v->out != NULL ||
      s->bytes_left == SIZE_MAX)
    return false;
  char *p = h2_pal_mem_alloc(v->mem, s->bytes_left + 1u);
  if (p == NULL)
    return false;
  const size_t n = s->bytes_left;
  if (!pb_read(s, (pb_byte_t *)p, n)) {
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

static int finish_unary_response(int rc,
                                 const h2_gizclaw_rpc_response_t *response) {
  if (rc != H2_PAL_OK || !response->has_error) {
    return rc;
  }
  switch (response->error_code) {
  case H2_GIZCLAW_RPC_ERROR_NOT_FOUND:
    return H2_PAL_ERR_NOT_FOUND;
  case H2_GIZCLAW_RPC_ERROR_METHOD_NOT_FOUND:
    return H2_PAL_ERR_UNSUPPORTED;
  case H2_GIZCLAW_RPC_ERROR_BAD_REQUEST:
  case H2_GIZCLAW_RPC_ERROR_INVALID_PARAMS:
    return H2_PAL_ERR_INVALID_ARG;
  case H2_GIZCLAW_RPC_ERROR_FORBIDDEN:
    return H2_PAL_ERR_IO;
  case H2_GIZCLAW_RPC_ERROR_CONFLICT:
    return H2_PAL_ERR_INVALID_STATE;
  default:
    return H2_PAL_ERR_IO;
  }
}

static int unary(h2_gizclaw_client_t *client, int method,
                 const pb_msgdesc_t *req_desc, const void *req,
                 h2_gizclaw_rpc_response_t *response) {
  const h2_pal_mem_api_t *mem = h2_gizclaw_client_allocator_internal(client);
  uint8_t *payload = NULL;
  size_t len = 0u;
  int rc = encode_message(mem, req_desc, req, &payload, &len);
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_client_rpc_call(
        client, method, (h2_gizclaw_rpc_bytes_t){payload, len}, response);
  h2_pal_mem_free(mem, payload);
  return finish_unary_response(rc, response);
}

void h2_gizclaw_pet_deinit(h2_gizclaw_client_t *client, h2_gizclaw_pet_t *pet) {
  if (client == NULL || pet == NULL)
    return;
  const h2_pal_mem_api_t *m = h2_gizclaw_client_allocator_internal(client);
  h2_pal_mem_free(m, pet->name);
  h2_pal_mem_free(m, pet->pet_def_name);
  h2_pal_mem_free(m, pet->display_name);
  h2_pal_mem_free(m, pet->workspace_name);
  h2_pal_mem_free(m, pet->died_at);
  h2_pal_mem_free(m, pet->state_settled_at);
  h2_pal_mem_free(m, pet->updated_at);
  memset(pet, 0, sizeof(*pet));
}

static int decode_pet_response(h2_gizclaw_client_t *client,
                               h2_gizclaw_rpc_response_t *response,
                               const pb_msgdesc_t *desc, void *wire_response,
                               gizclaw_rpc_v1_Pet *wire_pet, bool *has_pet,
                               h2_gizclaw_pet_t *out) {
  pet_decode_t ctx;
  prepare_pet(wire_pet, &ctx, h2_gizclaw_client_allocator_internal(client),
              out);
  pb_istream_t s = pb_istream_from_buffer(response->result_payload,
                                          response->result_payload_len);
  if (!pb_decode(&s, desc, wire_response) || !*has_pet) {
    h2_gizclaw_pet_deinit(client, out);
    return H2_PAL_ERR_FORMAT;
  }
  finish_pet(wire_pet, out);
  return H2_PAL_OK;
}

static bool valid_optional_text(h2_gizclaw_str_t value) {
  return value.len == 0u || value.data != NULL;
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

int h2_gizclaw_client_pet_get(h2_gizclaw_client_t *client,
                              h2_gizclaw_str_t pet_name,
                              h2_gizclaw_pet_t *out) {
  if (client == NULL || out == NULL || pet_name.data == NULL ||
      pet_name.len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_ServerPetGetRequest req =
      gizclaw_rpc_v1_ServerPetGetRequest_init_zero;
  req.has_value = true;
  text_arg_t name_arg;
  set_encoder(&req.value.name, &name_arg, pet_name);
  h2_gizclaw_rpc_response_t rsp = {0};
  int rc = unary(client, H2_GIZCLAW_RPC_SERVER_PET_GET,
                 gizclaw_rpc_v1_ServerPetGetRequest_fields, &req, &rsp);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPetGetResponse decoded =
        gizclaw_rpc_v1_ServerPetGetResponse_init_zero;
    rc = decode_pet_response(client, &rsp,
                             gizclaw_rpc_v1_ServerPetGetResponse_fields,
                             &decoded, &decoded.value, &decoded.has_value, out);
  }
  h2_gizclaw_rpc_response_deinit(client, &rsp);
  return rc;
}

int h2_gizclaw_client_pet_adopt(h2_gizclaw_client_t *client,
                                const h2_gizclaw_pet_adopt_options_t *options,
                                h2_gizclaw_pet_t *out) {
  if (client == NULL || options == NULL || out == NULL ||
      options->name.data == NULL || options->name.len == 0u ||
      !valid_utf8_span(options->name.data, options->name.len) ||
      !valid_optional_text(options->display_name))
    return H2_PAL_ERR_INVALID_ARG;
  gizclaw_rpc_v1_RuntimeAdoptRequest request =
      gizclaw_rpc_v1_RuntimeAdoptRequest_init_zero;
  request.has_value = true;
  text_arg_t text[2];
  set_encoder(&request.value.name, &text[0], options->name);
  if (options->display_name.len != 0u) {
    set_encoder(&request.value.display_name, &text[1], options->display_name);
  }
  h2_gizclaw_rpc_response_t rsp = {0};
  int rc = unary(client, H2_GIZCLAW_RPC_RUNTIME_ADOPT,
                 gizclaw_rpc_v1_RuntimeAdoptRequest_fields, &request, &rsp);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_RuntimeAdoptResponse decoded =
        gizclaw_rpc_v1_RuntimeAdoptResponse_init_zero;
    bool has_pet = false;
    pet_decode_t ctx;
    prepare_pet(&decoded.value.pet, &ctx,
                h2_gizclaw_client_allocator_internal(client), out);
    pb_istream_t s =
        pb_istream_from_buffer(rsp.result_payload, rsp.result_payload_len);
    if (!pb_decode(&s, gizclaw_rpc_v1_RuntimeAdoptResponse_fields, &decoded) ||
        !decoded.has_value || !decoded.value.has_pet)
      rc = H2_PAL_ERR_FORMAT;
    else {
      has_pet = true;
      finish_pet(&decoded.value.pet, out);
    }
    if (!has_pet)
      h2_gizclaw_pet_deinit(client, out);
  }
  h2_gizclaw_rpc_response_deinit(client, &rsp);
  return rc;
}

int h2_gizclaw_client_pet_delete(h2_gizclaw_client_t *client,
                                 h2_gizclaw_str_t pet_name,
                                 h2_gizclaw_pet_t *out_pet) {
  if (out_pet == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out_pet, 0, sizeof(*out_pet));
  if (client == NULL || pet_name.data == NULL || pet_name.len == 0u ||
      memchr(pet_name.data, '\0', pet_name.len) != NULL ||
      !valid_utf8_span(pet_name.data, pet_name.len)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  gizclaw_rpc_v1_ServerPetDeleteRequest request =
      gizclaw_rpc_v1_ServerPetDeleteRequest_init_zero;
  request.has_value = true;
  text_arg_t pet_name_arg;
  set_encoder(&request.value.name, &pet_name_arg, pet_name);
  h2_gizclaw_rpc_response_t response = {0};
  int rc =
      unary(client, H2_GIZCLAW_RPC_SERVER_PET_DELETE,
            gizclaw_rpc_v1_ServerPetDeleteRequest_fields, &request, &response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPetDeleteResponse decoded =
        gizclaw_rpc_v1_ServerPetDeleteResponse_init_zero;
    rc = decode_pet_response(
        client, &response, gizclaw_rpc_v1_ServerPetDeleteResponse_fields,
        &decoded, &decoded.value, &decoded.has_value, out_pet);
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  if (rc != H2_PAL_OK)
    h2_gizclaw_pet_deinit(client, out_pet);
  return rc;
}

static bool valid_game_result(const h2_gizclaw_pet_game_result_t *game_result) {
  return game_result != NULL && game_result->game_name.data != NULL &&
         game_result->game_name.len != 0u &&
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

int h2_gizclaw_client_pet_drive(h2_gizclaw_client_t *client,
                                const h2_gizclaw_pet_drive_options_t *options,
                                h2_gizclaw_pet_t *out_pet) {
  if (client == NULL || options == NULL || out_pet == NULL ||
      options->pet_name.data == NULL || options->pet_name.len == 0u ||
      !valid_optional_text(options->idempotency_key) ||
      options->behavior < H2_GIZCLAW_PET_BEHAVIOR_NONE ||
      options->behavior > H2_GIZCLAW_PET_BEHAVIOR_HEAL ||
      (options->behavior != H2_GIZCLAW_PET_BEHAVIOR_NONE &&
       options->game_result != NULL) ||
      (options->game_result != NULL &&
       !valid_game_result(options->game_result))) {
    return H2_PAL_ERR_INVALID_ARG;
  }

  gizclaw_rpc_v1_ServerPetDriveRequest request =
      gizclaw_rpc_v1_ServerPetDriveRequest_init_zero;
  request.has_value = true;
  text_arg_t pet_name_arg;
  text_arg_t drive_key_arg;
  text_arg_t game_text[5];
  set_encoder(&request.value.pet_name, &pet_name_arg, options->pet_name);
  if (options->behavior != H2_GIZCLAW_PET_BEHAVIOR_NONE) {
    request.value.has_behavior = true;
    request.value.behavior = (gizclaw_rpc_v1_PetBehavior)options->behavior;
  }
  if (options->game_result != NULL) {
    request.value.has_game_result = true;
    prepare_game_result(&request.value.game_result, options->game_result,
                        options->idempotency_key, game_text);
  } else if (options->idempotency_key.len != 0u) {
    set_encoder(&request.value.idempotency_key, &drive_key_arg,
                options->idempotency_key);
  }

  h2_gizclaw_rpc_response_t response = {0};
  int rc =
      unary(client, H2_GIZCLAW_RPC_SERVER_PET_DRIVE,
            gizclaw_rpc_v1_ServerPetDriveRequest_fields, &request, &response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPetDriveResponse decoded =
        gizclaw_rpc_v1_ServerPetDriveResponse_init_zero;
    pet_decode_t context;
    prepare_pet(&decoded.value.pet, &context,
                h2_gizclaw_client_allocator_internal(client), out_pet);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetDriveResponse_fields,
                   &decoded) ||
        !decoded.has_value || !decoded.value.has_pet) {
      h2_gizclaw_pet_deinit(client, out_pet);
      rc = H2_PAL_ERR_FORMAT;
    } else {
      finish_pet(&decoded.value.pet, out_pet);
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return rc;
}

typedef struct list_ctx {
  h2_gizclaw_client_t *client;
  h2_gizclaw_pet_page_t *page;
  size_t max;
} list_ctx_t;
static bool decode_list_pet(pb_istream_t *s, const pb_field_t *f, void **arg) {
  (void)f;
  list_ctx_t *c = *arg;
  if (c->page->count >= c->max || c->page->count == SIZE_MAX ||
      c->page->count + 1u > SIZE_MAX / sizeof(c->page->items[0]))
    return false;
  size_t n = c->page->count + 1u;
  h2_gizclaw_pet_t *items =
      h2_pal_mem_realloc(h2_gizclaw_client_allocator_internal(c->client),
                         c->page->items, n * sizeof(*items));
  if (!items)
    return false;
  c->page->items = items;
  gizclaw_rpc_v1_Pet p = gizclaw_rpc_v1_Pet_init_zero;
  pet_decode_t pc;
  prepare_pet(&p, &pc, h2_gizclaw_client_allocator_internal(c->client),
              &items[c->page->count]);
  if (!pb_decode(s, gizclaw_rpc_v1_Pet_fields, &p)) {
    h2_gizclaw_pet_deinit(c->client, &items[c->page->count]);
    return false;
  }
  finish_pet(&p, &items[c->page->count++]);
  return true;
}

int h2_gizclaw_client_pet_list(h2_gizclaw_client_t *client,
                               h2_gizclaw_str_t cursor, size_t limit,
                               h2_gizclaw_pet_page_t *out) {
  if (client == NULL || out == NULL || limit == 0u ||
#if SIZE_MAX > INT64_MAX
      limit > (size_t)INT64_MAX ||
#endif
      (cursor.len > 0u && cursor.data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  gizclaw_rpc_v1_ServerPetListRequest req =
      gizclaw_rpc_v1_ServerPetListRequest_init_zero;
  req.has_value = true;
  req.value.has_limit = true;
  req.value.limit = (int64_t)limit;
  text_arg_t cur_arg;
  if (cursor.len)
    set_encoder(&req.value.cursor, &cur_arg, cursor);
  h2_gizclaw_rpc_response_t rsp = {0};
  int rc = unary(client, H2_GIZCLAW_RPC_SERVER_PET_LIST,
                 gizclaw_rpc_v1_ServerPetListRequest_fields, &req, &rsp);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPetListResponse d =
        gizclaw_rpc_v1_ServerPetListResponse_init_zero;
    list_ctx_t lc = {client, out, limit};
    text_out_t next;
    d.value.items.funcs.decode = decode_list_pet;
    d.value.items.arg = &lc;
    set_decoder(&d.value.next_cursor, &next,
                h2_gizclaw_client_allocator_internal(client),
                &out->next_cursor);
    pb_istream_t s =
        pb_istream_from_buffer(rsp.result_payload, rsp.result_payload_len);
    if (!pb_decode(&s, gizclaw_rpc_v1_ServerPetListResponse_fields, &d) ||
        !d.has_value)
      rc = H2_PAL_ERR_FORMAT;
    else
      out->has_next = d.value.has_next;
  }
  h2_gizclaw_rpc_response_deinit(client, &rsp);
  if (rc != H2_PAL_OK)
    h2_gizclaw_pet_page_deinit(client, out);
  return rc;
}

void h2_gizclaw_pet_page_deinit(h2_gizclaw_client_t *client,
                                h2_gizclaw_pet_page_t *p) {
  if (!client || !p)
    return;
  const h2_pal_mem_api_t *m = h2_gizclaw_client_allocator_internal(client);
  for (size_t i = 0; i < p->count; ++i)
    h2_gizclaw_pet_deinit(client, &p->items[i]);
  h2_pal_mem_free(m, p->items);
  h2_pal_mem_free(m, p->next_cursor);
  memset(p, 0, sizeof(*p));
}

typedef struct actions_clip_decode {
  h2_gizclaw_client_t *client;
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
  if (context == NULL || context->client == NULL || context->actions == NULL) {
    return false;
  }
  h2_gizclaw_pet_actions_t *actions = context->actions;
  const size_t count = actions->clip_name_count + 1u;
  const h2_pal_mem_api_t *mem =
      h2_gizclaw_client_allocator_internal(context->client);
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
                            h2_gizclaw_client_t *client,
                            h2_gizclaw_pet_actions_t *actions) {
  const h2_pal_mem_api_t *mem = h2_gizclaw_client_allocator_internal(client);
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
  context->clips.client = client;
  context->clips.actions = actions;
  wire->clip_names.funcs.decode = decode_action_clip;
  wire->clip_names.arg = &context->clips;
}

int h2_gizclaw_client_pet_actions_get(h2_gizclaw_client_t *client,
                                      h2_gizclaw_str_t pet_name,
                                      h2_gizclaw_pet_actions_t *out_actions) {
  if (client == NULL || pet_name.data == NULL || pet_name.len == 0u ||
      out_actions == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  gizclaw_rpc_v1_ServerPetActionsGetRequest request =
      gizclaw_rpc_v1_ServerPetActionsGetRequest_init_zero;
  request.has_value = true;
  text_arg_t pet_name_arg;
  set_encoder(&request.value.name, &pet_name_arg, pet_name);
  h2_gizclaw_rpc_response_t response = {0};
  int rc = unary(client, H2_GIZCLAW_RPC_SERVER_PET_ACTIONS_GET,
                 gizclaw_rpc_v1_ServerPetActionsGetRequest_fields, &request,
                 &response);
  if (rc == H2_PAL_OK) {
    gizclaw_rpc_v1_ServerPetActionsGetResponse decoded =
        gizclaw_rpc_v1_ServerPetActionsGetResponse_init_zero;
    actions_decode_t context;
    prepare_actions(&decoded.value, &context, client, out_actions);
    pb_istream_t stream = pb_istream_from_buffer(response.result_payload,
                                                 response.result_payload_len);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetActionsGetResponse_fields,
                   &decoded) ||
        !decoded.has_value) {
      h2_gizclaw_pet_actions_deinit(client, out_actions);
      rc = H2_PAL_ERR_FORMAT;
    }
  }
  h2_gizclaw_rpc_response_deinit(client, &response);
  return rc;
}

const char *
h2_gizclaw_pet_actions_find_clip(const h2_gizclaw_pet_actions_t *actions,
                                 const char *id) {
  if (actions == NULL || id == NULL) {
    return NULL;
  }
  for (size_t index = 0u; index < actions->clip_name_count; ++index) {
    if (actions->clip_names[index].id != NULL &&
        strcmp(actions->clip_names[index].id, id) == 0) {
      return actions->clip_names[index].pixa_clip_name;
    }
  }
  return NULL;
}

void h2_gizclaw_pet_actions_deinit(h2_gizclaw_client_t *client,
                                   h2_gizclaw_pet_actions_t *actions) {
  if (client == NULL || actions == NULL) {
    return;
  }
  const h2_pal_mem_api_t *mem = h2_gizclaw_client_allocator_internal(client);
  h2_pal_mem_free(mem, actions->pet_name);
  h2_pal_mem_free(mem, actions->pet_def_name);
  h2_pal_mem_free(mem, actions->feed);
  h2_pal_mem_free(mem, actions->bathe);
  h2_pal_mem_free(mem, actions->play);
  h2_pal_mem_free(mem, actions->heal);
  h2_pal_mem_free(mem, actions->idle);
  h2_pal_mem_free(mem, actions->sick);
  h2_pal_mem_free(mem, actions->dead);
  h2_pal_mem_free(mem, actions->sleep);
  for (size_t index = 0u; index < actions->clip_name_count; ++index) {
    h2_pal_mem_free(mem, actions->clip_names[index].id);
    h2_pal_mem_free(mem, actions->clip_names[index].pixa_clip_name);
  }
  h2_pal_mem_free(mem, actions->clip_names);
  memset(actions, 0, sizeof(*actions));
}

typedef struct download_ctx {
  h2_gizclaw_client_t *client;
  h2_gizclaw_pet_pixa_write_fn write;
  void *user;
  h2_gizclaw_pet_pixa_info_t *info;
  int result;
} download_ctx_t;
static int download_event(void *user, const h2_gizclaw_rpc_stream_event_t *e) {
  download_ctx_t *c = user;
  if (e->has_error)
    return c->result = H2_PAL_ERR_IO;
  if (e->kind == H2_GIZCLAW_RPC_STREAM_RESPONSE) {
    gizclaw_rpc_v1_ServerPetPixaDownloadResponse d =
        gizclaw_rpc_v1_ServerPetPixaDownloadResponse_init_zero;
    text_out_t text[3];
    const h2_pal_mem_api_t *m = h2_gizclaw_client_allocator_internal(c->client);
    set_decoder(&d.value.pet_name, &text[0], m, &c->info->pet_name);
    set_decoder(&d.value.pet_def_name, &text[1], m, &c->info->pet_def_name);
    set_decoder(&d.value.pixa_path, &text[2], m, &c->info->source_path);
    pb_istream_t s =
        pb_istream_from_buffer(e->result_payload.data, e->result_payload.len);
    if (!pb_decode(&s, gizclaw_rpc_v1_ServerPetPixaDownloadResponse_fields,
                   &d) ||
        !d.has_value || d.value.size_bytes < 0)
      return c->result = H2_PAL_ERR_FORMAT;
    c->info->size_bytes = (uint64_t)d.value.size_bytes;
  } else if (e->kind == H2_GIZCLAW_RPC_STREAM_DATA) {
    if (c->info->received_bytes > c->info->size_bytes ||
        e->data.len > c->info->size_bytes - c->info->received_bytes)
      return c->result = H2_PAL_ERR_FORMAT;
    if (c->write(c->user, e->data.data, e->data.len) != 0)
      return c->result = H2_PAL_ERR_IO;
    c->info->received_bytes += e->data.len;
  }
  return 0;
}

int h2_gizclaw_client_pet_pixa_download(h2_gizclaw_client_t *client,
                                        h2_gizclaw_str_t id,
                                        h2_gizclaw_pet_pixa_write_fn write,
                                        void *user,
                                        h2_gizclaw_pet_pixa_info_t *out) {
  if (!client || !id.data || !id.len || !write || !out)
    return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  gizclaw_rpc_v1_ServerPetPixaDownloadRequest req =
      gizclaw_rpc_v1_ServerPetPixaDownloadRequest_init_zero;
  req.has_value = true;
  text_arg_t id_arg;
  set_encoder(&req.value.pet_name, &id_arg, id);
  const h2_pal_mem_api_t *m = h2_gizclaw_client_allocator_internal(client);
  uint8_t *payload = NULL;
  size_t len = 0;
  int rc = encode_message(m, gizclaw_rpc_v1_ServerPetPixaDownloadRequest_fields,
                          &req, &payload, &len);
  download_ctx_t ctx = {client, write, user, out, H2_PAL_OK};
  if (rc == H2_PAL_OK)
    rc = h2_gizclaw_client_rpc_call_stream(
        client, H2_GIZCLAW_RPC_SERVER_PET_PIXA_DOWNLOAD,
        (h2_gizclaw_rpc_bytes_t){payload, len}, download_event, &ctx);
  h2_pal_mem_free(m, payload);
  if (rc == H2_PAL_OK)
    rc = ctx.result;
  if (rc == H2_PAL_OK && out->received_bytes != out->size_bytes)
    rc = H2_PAL_ERR_FORMAT;
  if (rc != H2_PAL_OK)
    h2_gizclaw_pet_pixa_info_deinit(client, out);
  return rc;
}

void h2_gizclaw_pet_pixa_info_deinit(h2_gizclaw_client_t *client,
                                     h2_gizclaw_pet_pixa_info_t *i) {
  if (!client || !i)
    return;
  const h2_pal_mem_api_t *m = h2_gizclaw_client_allocator_internal(client);
  h2_pal_mem_free(m, i->pet_name);
  h2_pal_mem_free(m, i->pet_def_name);
  h2_pal_mem_free(m, i->source_path);
  memset(i, 0, sizeof(*i));
}

struct h2_gizclaw_pet_request {
  h2_gizclaw_service_t *service;
  const h2_pal_mem_api_t *allocator;
  h2_gizclaw_async_rpc_t *rpc;
  h2_gizclaw_async_stream_t *stream;
  h2_gizclaw_pet_request_kind_t kind;
  h2_gizclaw_pet_completion_fn completion;
  void *completion_user;
  h2_gizclaw_operation_result_t operation_result;
  h2_gizclaw_pet_result_t result;
  char *text[6];
  size_t limit;
  h2_gizclaw_pet_adopt_options_t adopt;
  h2_gizclaw_pet_drive_options_t drive;
  h2_gizclaw_pet_game_result_t game;
  h2_gizclaw_pet_pixa_write_fn write;
  void *write_user;
  atomic_bool terminal;
};

static h2_gizclaw_str_t pet_request_span(const char *text) {
  return (h2_gizclaw_str_t){.data = text,
                            .len = text == NULL ? 0u : strlen(text)};
}

static char *pet_request_copy(const h2_pal_mem_api_t *allocator,
                              h2_gizclaw_str_t value) {
  if (value.len > 0u && value.data == NULL)
    return NULL;
  char *copy = h2_pal_mem_alloc(allocator, value.len + 1u);
  if (copy == NULL)
    return NULL;
  if (value.len > 0u)
    memcpy(copy, value.data, value.len);
  copy[value.len] = '\0';
  return copy;
}

static void pet_request_pet_clear(const h2_pal_mem_api_t *allocator,
                                  h2_gizclaw_pet_t *pet) {
  h2_pal_mem_free(allocator, pet->name);
  h2_pal_mem_free(allocator, pet->pet_def_name);
  h2_pal_mem_free(allocator, pet->display_name);
  h2_pal_mem_free(allocator, pet->workspace_name);
  h2_pal_mem_free(allocator, pet->died_at);
  h2_pal_mem_free(allocator, pet->state_settled_at);
  h2_pal_mem_free(allocator, pet->updated_at);
  memset(pet, 0, sizeof(*pet));
}

static void pet_request_result_clear(h2_gizclaw_pet_request_t *request) {
  if (request->kind == H2_GIZCLAW_PET_LIST) {
    h2_gizclaw_pet_page_t *page = &request->result.value.page;
    for (size_t index = 0u; index < page->count; ++index)
      pet_request_pet_clear(request->allocator, &page->items[index]);
    h2_pal_mem_free(request->allocator, page->items);
    h2_pal_mem_free(request->allocator, page->next_cursor);
    memset(page, 0, sizeof(*page));
  } else if (request->kind == H2_GIZCLAW_PET_PIXA_DOWNLOAD) {
    h2_gizclaw_pet_pixa_info_t *info = &request->result.value.pixa;
    h2_pal_mem_free(request->allocator, info->pet_name);
    h2_pal_mem_free(request->allocator, info->pet_def_name);
    h2_pal_mem_free(request->allocator, info->source_path);
    memset(info, 0, sizeof(*info));
  } else if (request->kind == H2_GIZCLAW_PET_ACTIONS_GET) {
    h2_gizclaw_pet_actions_t *actions = &request->result.value.actions;
    h2_pal_mem_free(request->allocator, actions->pet_name);
    h2_pal_mem_free(request->allocator, actions->pet_def_name);
    h2_pal_mem_free(request->allocator, actions->feed);
    h2_pal_mem_free(request->allocator, actions->bathe);
    h2_pal_mem_free(request->allocator, actions->play);
    h2_pal_mem_free(request->allocator, actions->heal);
    h2_pal_mem_free(request->allocator, actions->idle);
    h2_pal_mem_free(request->allocator, actions->sick);
    h2_pal_mem_free(request->allocator, actions->dead);
    h2_pal_mem_free(request->allocator, actions->sleep);
    for (size_t index = 0u; index < actions->clip_name_count; ++index) {
      h2_pal_mem_free(request->allocator, actions->clip_names[index].id);
      h2_pal_mem_free(request->allocator,
                      actions->clip_names[index].pixa_clip_name);
    }
    h2_pal_mem_free(request->allocator, actions->clip_names);
    memset(actions, 0, sizeof(*actions));
  } else {
    pet_request_pet_clear(request->allocator, &request->result.value.pet);
  }
}

static h2_pal_result_t
pet_decode_response(h2_gizclaw_pet_request_t *request,
                    const h2_gizclaw_rpc_response_t *response) {
  h2_gizclaw_client_t *client = request->service->client;
  if (client == NULL || response == NULL)
    return H2_PAL_ERR_INVALID_STATE;
  h2_pal_result_t rc =
      (h2_pal_result_t)finish_unary_response(H2_PAL_OK, response);
  if (rc != H2_PAL_OK)
    return rc;
  pb_istream_t stream = pb_istream_from_buffer(response->result_payload,
                                               response->result_payload_len);
  switch (request->kind) {
  case H2_GIZCLAW_PET_LIST: {
    gizclaw_rpc_v1_ServerPetListResponse decoded =
        gizclaw_rpc_v1_ServerPetListResponse_init_zero;
    list_ctx_t context = {client, &request->result.value.page, request->limit};
    text_out_t next;
    decoded.value.items.funcs.decode = decode_list_pet;
    decoded.value.items.arg = &context;
    set_decoder(&decoded.value.next_cursor, &next,
                h2_gizclaw_client_allocator_internal(client),
                &request->result.value.page.next_cursor);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetListResponse_fields,
                   &decoded) ||
        !decoded.has_value)
      return H2_PAL_ERR_FORMAT;
    request->result.value.page.has_next = decoded.value.has_next;
    return H2_PAL_OK;
  }
  case H2_GIZCLAW_PET_GET: {
    gizclaw_rpc_v1_ServerPetGetResponse decoded =
        gizclaw_rpc_v1_ServerPetGetResponse_init_zero;
    return (h2_pal_result_t)decode_pet_response(
        client, (h2_gizclaw_rpc_response_t *)response,
        gizclaw_rpc_v1_ServerPetGetResponse_fields, &decoded, &decoded.value,
        &decoded.has_value, &request->result.value.pet);
  }
  case H2_GIZCLAW_PET_DELETE: {
    gizclaw_rpc_v1_ServerPetDeleteResponse decoded =
        gizclaw_rpc_v1_ServerPetDeleteResponse_init_zero;
    return (h2_pal_result_t)decode_pet_response(
        client, (h2_gizclaw_rpc_response_t *)response,
        gizclaw_rpc_v1_ServerPetDeleteResponse_fields, &decoded, &decoded.value,
        &decoded.has_value, &request->result.value.pet);
  }
  case H2_GIZCLAW_PET_ADOPT: {
    gizclaw_rpc_v1_RuntimeAdoptResponse decoded =
        gizclaw_rpc_v1_RuntimeAdoptResponse_init_zero;
    pet_decode_t context;
    prepare_pet(&decoded.value.pet, &context,
                h2_gizclaw_client_allocator_internal(client),
                &request->result.value.pet);
    if (!pb_decode(&stream, gizclaw_rpc_v1_RuntimeAdoptResponse_fields,
                   &decoded) ||
        !decoded.has_value || !decoded.value.has_pet)
      return H2_PAL_ERR_FORMAT;
    finish_pet(&decoded.value.pet, &request->result.value.pet);
    return H2_PAL_OK;
  }
  case H2_GIZCLAW_PET_DRIVE: {
    gizclaw_rpc_v1_ServerPetDriveResponse decoded =
        gizclaw_rpc_v1_ServerPetDriveResponse_init_zero;
    pet_decode_t context;
    prepare_pet(&decoded.value.pet, &context,
                h2_gizclaw_client_allocator_internal(client),
                &request->result.value.pet);
    if (!pb_decode(&stream, gizclaw_rpc_v1_ServerPetDriveResponse_fields,
                   &decoded) ||
        !decoded.has_value || !decoded.value.has_pet)
      return H2_PAL_ERR_FORMAT;
    finish_pet(&decoded.value.pet, &request->result.value.pet);
    return H2_PAL_OK;
  }
  case H2_GIZCLAW_PET_ACTIONS_GET: {
    gizclaw_rpc_v1_ServerPetActionsGetResponse decoded =
        gizclaw_rpc_v1_ServerPetActionsGetResponse_init_zero;
    actions_decode_t context;
    prepare_actions(&decoded.value, &context, client,
                    &request->result.value.actions);
    return pb_decode(&stream, gizclaw_rpc_v1_ServerPetActionsGetResponse_fields,
                     &decoded) &&
                   decoded.has_value
               ? H2_PAL_OK
               : H2_PAL_ERR_FORMAT;
  }
  case H2_GIZCLAW_PET_PIXA_DOWNLOAD:
    return H2_PAL_ERR_INVALID_STATE;
  }
  return H2_PAL_ERR_INVALID_STATE;
}

static void
pet_request_finish(h2_gizclaw_pet_request_t *request,
                   const h2_gizclaw_operation_result_t *operation_result,
                   h2_pal_result_t result) {
  h2_gizclaw_operation_result_t completed = *operation_result;
  completed.result = result;
  if (result != H2_PAL_OK)
    pet_request_result_clear(request);
  request->operation_result = completed;
  atomic_store_explicit(&request->terminal, true, memory_order_release);
  request->completion(request->completion_user, request);
}

static void pet_rpc_complete(void *user, h2_gizclaw_async_rpc_t *rpc) {
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_async_rpc_operation_result(rpc);
  const h2_gizclaw_rpc_response_t *response =
      h2_gizclaw_async_rpc_response(rpc);
  h2_gizclaw_pet_request_t *request = user;
  const h2_pal_result_t result = operation_result->result == H2_PAL_OK
                                     ? pet_decode_response(request, response)
                                     : operation_result->result;
  pet_request_finish(request, operation_result, result);
}

static h2_pal_result_t
pet_stream_event(void *user, h2_gizclaw_async_stream_t *stream,
                 const h2_gizclaw_rpc_stream_event_t *event) {
  (void)stream;
  h2_gizclaw_pet_request_t *request = user;
  download_ctx_t context = {
      .client = request->service->client,
      .write = request->write,
      .user = request->write_user,
      .info = &request->result.value.pixa,
      .result = H2_PAL_OK,
  };
  return (h2_pal_result_t)download_event(&context, event);
}

static void
pet_stream_complete(void *user, h2_gizclaw_async_stream_t *stream) {
  const h2_gizclaw_operation_result_t *operation_result =
      h2_gizclaw_async_stream_operation_result(stream);
  h2_gizclaw_pet_request_t *request = user;
  h2_pal_result_t result = operation_result->result;
  if (result == H2_PAL_OK && request->result.value.pixa.received_bytes !=
                                 request->result.value.pixa.size_bytes)
    result = H2_PAL_ERR_FORMAT;
  pet_request_finish(request, operation_result, result);
}

static h2_gizclaw_pet_request_t *
pet_request_allocate(h2_gizclaw_service_t *service,
                     h2_gizclaw_pet_request_kind_t kind,
                     h2_gizclaw_pet_completion_fn completion, void *user) {
  if (service == NULL || completion == NULL)
    return NULL;
  const h2_pal_mem_api_t *allocator = service->config.client_config->allocator;
  h2_gizclaw_pet_request_t *request =
      h2_pal_mem_alloc(allocator, sizeof(*request));
  if (request == NULL)
    return NULL;
  memset(request, 0, sizeof(*request));
  request->service = service;
  request->allocator = allocator;
  request->kind = kind;
  request->result.kind = kind;
  request->completion = completion;
  request->completion_user = user;
  return request;
}

static void pet_request_free_unsubmitted(h2_gizclaw_pet_request_t *request) {
  if (request == NULL)
    return;
  for (size_t index = 0u; index < 6u; ++index)
    h2_pal_mem_free(request->allocator, request->text[index]);
  h2_pal_mem_free(request->allocator, request);
}

static h2_pal_result_t
pet_request_submit(h2_gizclaw_service_t *service, uint64_t identity,
                   h2_gizclaw_pet_request_t *request,
                   h2_gizclaw_pet_request_t **out_request) {
  const pb_msgdesc_t *description = NULL;
  const void *message = NULL;
  int method = 0;
  gizclaw_rpc_v1_ServerPetListRequest list =
      gizclaw_rpc_v1_ServerPetListRequest_init_zero;
  gizclaw_rpc_v1_ServerPetGetRequest get =
      gizclaw_rpc_v1_ServerPetGetRequest_init_zero;
  gizclaw_rpc_v1_RuntimeAdoptRequest adopt =
      gizclaw_rpc_v1_RuntimeAdoptRequest_init_zero;
  gizclaw_rpc_v1_ServerPetDeleteRequest delete_request =
      gizclaw_rpc_v1_ServerPetDeleteRequest_init_zero;
  gizclaw_rpc_v1_ServerPetDriveRequest drive =
      gizclaw_rpc_v1_ServerPetDriveRequest_init_zero;
  gizclaw_rpc_v1_ServerPetActionsGetRequest actions =
      gizclaw_rpc_v1_ServerPetActionsGetRequest_init_zero;
  gizclaw_rpc_v1_ServerPetPixaDownloadRequest pixa =
      gizclaw_rpc_v1_ServerPetPixaDownloadRequest_init_zero;
  text_arg_t text[7];
  memset(text, 0, sizeof(text));

  switch (request->kind) {
  case H2_GIZCLAW_PET_LIST:
    list.has_value = true;
    list.value.has_limit = true;
    list.value.limit = (int64_t)request->limit;
    if (request->text[0][0] != '\0')
      set_encoder(&list.value.cursor, &text[0],
                  pet_request_span(request->text[0]));
    method = H2_GIZCLAW_RPC_SERVER_PET_LIST;
    description = gizclaw_rpc_v1_ServerPetListRequest_fields;
    message = &list;
    break;
  case H2_GIZCLAW_PET_GET:
    get.has_value = true;
    set_encoder(&get.value.name, &text[0], pet_request_span(request->text[0]));
    method = H2_GIZCLAW_RPC_SERVER_PET_GET;
    description = gizclaw_rpc_v1_ServerPetGetRequest_fields;
    message = &get;
    break;
  case H2_GIZCLAW_PET_ADOPT:
    adopt.has_value = true;
    set_encoder(&adopt.value.name, &text[0], request->adopt.name);
    if (request->adopt.display_name.len > 0u)
      set_encoder(&adopt.value.display_name, &text[1],
                  request->adopt.display_name);
    method = H2_GIZCLAW_RPC_RUNTIME_ADOPT;
    description = gizclaw_rpc_v1_RuntimeAdoptRequest_fields;
    message = &adopt;
    break;
  case H2_GIZCLAW_PET_DELETE:
    delete_request.has_value = true;
    set_encoder(&delete_request.value.name, &text[0],
                pet_request_span(request->text[0]));
    method = H2_GIZCLAW_RPC_SERVER_PET_DELETE;
    description = gizclaw_rpc_v1_ServerPetDeleteRequest_fields;
    message = &delete_request;
    break;
  case H2_GIZCLAW_PET_DRIVE:
    drive.has_value = true;
    set_encoder(&drive.value.pet_name, &text[0], request->drive.pet_name);
    if (request->drive.behavior != H2_GIZCLAW_PET_BEHAVIOR_NONE) {
      drive.value.has_behavior = true;
      drive.value.behavior =
          (gizclaw_rpc_v1_PetBehavior)request->drive.behavior;
    }
    if (request->drive.game_result != NULL) {
      drive.value.has_game_result = true;
      prepare_game_result(&drive.value.game_result, request->drive.game_result,
                          request->drive.idempotency_key, &text[1]);
    } else if (request->drive.idempotency_key.len > 0u) {
      set_encoder(&drive.value.idempotency_key, &text[1],
                  request->drive.idempotency_key);
    }
    method = H2_GIZCLAW_RPC_SERVER_PET_DRIVE;
    description = gizclaw_rpc_v1_ServerPetDriveRequest_fields;
    message = &drive;
    break;
  case H2_GIZCLAW_PET_ACTIONS_GET:
    actions.has_value = true;
    set_encoder(&actions.value.name, &text[0],
                pet_request_span(request->text[0]));
    method = H2_GIZCLAW_RPC_SERVER_PET_ACTIONS_GET;
    description = gizclaw_rpc_v1_ServerPetActionsGetRequest_fields;
    message = &actions;
    break;
  case H2_GIZCLAW_PET_PIXA_DOWNLOAD:
    pixa.has_value = true;
    set_encoder(&pixa.value.pet_name, &text[0],
                pet_request_span(request->text[0]));
    method = H2_GIZCLAW_RPC_SERVER_PET_PIXA_DOWNLOAD;
    description = gizclaw_rpc_v1_ServerPetPixaDownloadRequest_fields;
    message = &pixa;
    break;
  }

  uint8_t *payload = NULL;
  size_t payload_len = 0u;
  h2_pal_result_t rc = (h2_pal_result_t)encode_message(
      request->allocator, description, message, &payload, &payload_len);
  if (rc == H2_PAL_OK && request->kind == H2_GIZCLAW_PET_PIXA_DOWNLOAD) {
    rc = h2_gizclaw_service_rpc_stream_async(
        service, identity, (h2_gizclaw_rpc_method_t)method,
        (h2_gizclaw_rpc_bytes_t){payload, payload_len}, 30000u,
        pet_stream_event, pet_stream_complete, request, &request->stream);
  } else if (rc == H2_PAL_OK) {
    rc = h2_gizclaw_service_rpc_call_async(
        service, identity, (h2_gizclaw_rpc_method_t)method,
        (h2_gizclaw_rpc_bytes_t){payload, payload_len}, 30000u,
        pet_rpc_complete, request, &request->rpc);
  }
  h2_pal_mem_free(request->allocator, payload);
  if (rc != H2_PAL_OK) {
    pet_request_free_unsubmitted(request);
    return rc;
  }
  *out_request = request;
  return H2_PAL_OK;
}

static h2_pal_result_t pet_request_copy_at(h2_gizclaw_pet_request_t *request,
                                           size_t index,
                                           h2_gizclaw_str_t value) {
  request->text[index] = pet_request_copy(request->allocator, value);
  return request->text[index] == NULL ? H2_PAL_ERR_NO_MEMORY : H2_PAL_OK;
}

#define PET_ASYNC_VALIDATE(service, completion, out_request)                   \
  do {                                                                         \
    if ((out_request) != NULL)                                                 \
      *(out_request) = NULL;                                                   \
    if ((service) == NULL || (completion) == NULL || (out_request) == NULL)    \
      return H2_PAL_ERR_INVALID_ARG;                                           \
  } while (0)

h2_pal_result_t h2_gizclaw_service_pet_list_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t cursor,
    size_t limit, h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  PET_ASYNC_VALIDATE(service, completion, out_request);
  if (limit == 0u ||
#if SIZE_MAX > INT64_MAX
      limit > (size_t)INT64_MAX ||
#endif
      (cursor.len > 0u && cursor.data == NULL))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_pet_request_t *request =
      pet_request_allocate(service, H2_GIZCLAW_PET_LIST, completion, user);
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  request->limit = limit;
  if (pet_request_copy_at(request, 0u, cursor) != H2_PAL_OK) {
    pet_request_free_unsubmitted(request);
    return H2_PAL_ERR_NO_MEMORY;
  }
  return pet_request_submit(service, identity, request, out_request);
}

static h2_pal_result_t pet_single_name_submit(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_request_kind_t kind, h2_gizclaw_pet_completion_fn completion,
    void *user, h2_gizclaw_pet_request_t **out_request) {
  PET_ASYNC_VALIDATE(service, completion, out_request);
  if (pet_name.data == NULL || pet_name.len == 0u ||
      memchr(pet_name.data, '\0', pet_name.len) != NULL ||
      !valid_utf8_span(pet_name.data, pet_name.len))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_pet_request_t *request =
      pet_request_allocate(service, kind, completion, user);
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  if (pet_request_copy_at(request, 0u, pet_name) != H2_PAL_OK) {
    pet_request_free_unsubmitted(request);
    return H2_PAL_ERR_NO_MEMORY;
  }
  return pet_request_submit(service, identity, request, out_request);
}

h2_pal_result_t h2_gizclaw_service_pet_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  return pet_single_name_submit(service, identity, pet_name, H2_GIZCLAW_PET_GET,
                                completion, user, out_request);
}

h2_pal_result_t h2_gizclaw_service_pet_delete_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  return pet_single_name_submit(service, identity, pet_name,
                                H2_GIZCLAW_PET_DELETE, completion, user,
                                out_request);
}

h2_pal_result_t h2_gizclaw_service_pet_actions_get_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  return pet_single_name_submit(service, identity, pet_name,
                                H2_GIZCLAW_PET_ACTIONS_GET, completion, user,
                                out_request);
}

h2_pal_result_t h2_gizclaw_service_pet_adopt_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_adopt_options_t *options,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  PET_ASYNC_VALIDATE(service, completion, out_request);
  if (options == NULL || options->name.data == NULL ||
      options->name.len == 0u ||
      !valid_utf8_span(options->name.data, options->name.len) ||
      !valid_optional_text(options->display_name))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_pet_request_t *request =
      pet_request_allocate(service, H2_GIZCLAW_PET_ADOPT, completion, user);
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  if (pet_request_copy_at(request, 0u, options->name) != H2_PAL_OK ||
      pet_request_copy_at(request, 1u, options->display_name) != H2_PAL_OK) {
    pet_request_free_unsubmitted(request);
    return H2_PAL_ERR_NO_MEMORY;
  }
  request->adopt.name = pet_request_span(request->text[0]);
  request->adopt.display_name = pet_request_span(request->text[1]);
  return pet_request_submit(service, identity, request, out_request);
}

h2_pal_result_t h2_gizclaw_service_pet_drive_async(
    h2_gizclaw_service_t *service, uint64_t identity,
    const h2_gizclaw_pet_drive_options_t *options,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  PET_ASYNC_VALIDATE(service, completion, out_request);
  if (options == NULL || options->pet_name.data == NULL ||
      options->pet_name.len == 0u ||
      !valid_optional_text(options->idempotency_key) ||
      options->behavior < H2_GIZCLAW_PET_BEHAVIOR_NONE ||
      options->behavior > H2_GIZCLAW_PET_BEHAVIOR_HEAL ||
      (options->behavior != H2_GIZCLAW_PET_BEHAVIOR_NONE &&
       options->game_result != NULL) ||
      (options->game_result != NULL &&
       !valid_game_result(options->game_result)))
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_pet_request_t *request =
      pet_request_allocate(service, H2_GIZCLAW_PET_DRIVE, completion, user);
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  const h2_gizclaw_pet_game_result_t *game = options->game_result;
  const h2_gizclaw_str_t values[6] = {
      options->pet_name,
      options->idempotency_key,
      game != NULL ? game->game_name : (h2_gizclaw_str_t){0},
      game != NULL ? game->difficulty : (h2_gizclaw_str_t){0},
      game != NULL ? game->outcome : (h2_gizclaw_str_t){0},
      game != NULL ? game->occurred_at : (h2_gizclaw_str_t){0},
  };
  for (size_t index = 0u; index < 6u; ++index) {
    if (pet_request_copy_at(request, index, values[index]) != H2_PAL_OK) {
      pet_request_free_unsubmitted(request);
      return H2_PAL_ERR_NO_MEMORY;
    }
  }
  request->drive = *options;
  request->drive.pet_name = pet_request_span(request->text[0]);
  request->drive.idempotency_key = pet_request_span(request->text[1]);
  if (game != NULL) {
    request->game = *game;
    request->game.game_name = pet_request_span(request->text[2]);
    request->game.difficulty = pet_request_span(request->text[3]);
    request->game.outcome = pet_request_span(request->text[4]);
    request->game.occurred_at = pet_request_span(request->text[5]);
    request->drive.game_result = &request->game;
  }
  return pet_request_submit(service, identity, request, out_request);
}

h2_pal_result_t h2_gizclaw_service_pet_pixa_download_async(
    h2_gizclaw_service_t *service, uint64_t identity, h2_gizclaw_str_t pet_name,
    h2_gizclaw_pet_pixa_write_fn write, void *write_user,
    h2_gizclaw_pet_completion_fn completion, void *user,
    h2_gizclaw_pet_request_t **out_request) {
  PET_ASYNC_VALIDATE(service, completion, out_request);
  if (write == NULL || pet_name.data == NULL || pet_name.len == 0u)
    return H2_PAL_ERR_INVALID_ARG;
  h2_gizclaw_pet_request_t *request = pet_request_allocate(
      service, H2_GIZCLAW_PET_PIXA_DOWNLOAD, completion, user);
  if (request == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  request->write = write;
  request->write_user = write_user;
  if (pet_request_copy_at(request, 0u, pet_name) != H2_PAL_OK) {
    pet_request_free_unsubmitted(request);
    return H2_PAL_ERR_NO_MEMORY;
  }
  return pet_request_submit(service, identity, request, out_request);
}

h2_pal_result_t
h2_gizclaw_pet_request_cancel(h2_gizclaw_pet_request_t *request) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->stream != NULL
             ? h2_gizclaw_async_stream_cancel(request->stream)
             : h2_gizclaw_async_rpc_cancel(request->rpc);
}

h2_pal_result_t h2_gizclaw_pet_request_wait(
    h2_gizclaw_pet_request_t *request, uint32_t timeout_ms) {
  if (request == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  return request->stream != NULL
             ? h2_gizclaw_async_stream_wait(request->stream, timeout_ms)
             : h2_gizclaw_async_rpc_wait(request->rpc, timeout_ms);
}

const h2_gizclaw_operation_result_t *h2_gizclaw_pet_request_operation_result(
    const h2_gizclaw_pet_request_t *request) {
  return request != NULL &&
                 atomic_load_explicit(&request->terminal, memory_order_acquire)
             ? &request->operation_result
             : NULL;
}

const h2_gizclaw_pet_result_t *h2_gizclaw_pet_request_response(
    const h2_gizclaw_pet_request_t *request) {
  const h2_gizclaw_operation_result_t *result =
      h2_gizclaw_pet_request_operation_result(request);
  return result != NULL && result->result == H2_PAL_OK ? &request->result
                                                       : NULL;
}

void h2_gizclaw_pet_request_release(h2_gizclaw_pet_request_t *request) {
  if (request == NULL ||
      !atomic_load_explicit(&request->terminal, memory_order_acquire))
    return;
  h2_gizclaw_async_rpc_release(request->rpc);
  h2_gizclaw_async_stream_release(request->stream);
  pet_request_result_clear(request);
  pet_request_free_unsubmitted(request);
}

#undef PET_ASYNC_VALIDATE
