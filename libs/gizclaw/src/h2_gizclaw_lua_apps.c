#include "h2_gizclaw_lua_apps_internal.h"
#include "payload/lua_app.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "h2_atomic.h"
#include <string.h>

typedef struct lua_run {
  h2_gizclaw_lua_app_run_t value;
  size_t used, text_bytes;
  char strings[H2_GIZCLAW_LUA_APP_PARAMS_BYTES_MAX +
               2u * H2_GIZCLAW_LUA_APP_PARAMS_MAX];
} lua_run_t;

struct h2_gizclaw_lua_apps {
  h2_gizclaw_config_t config;
  h2_gizclaw_lua_app_handoff_t handoff;
  h2_atomic_bool_t stopping;
  uint8_t *response;
  size_t capacity;
};

static bool utf8(const char *str, size_t size) {
  const unsigned char *p = (const unsigned char *)str;
  for (size_t i = 0; i < size;) {
    unsigned c = p[i++];
    if (c < 0x80) {
      if (!c)
        return false;
      continue;
    }
    unsigned n, value, minimum;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      value = c & 31u;
      minimum = 0x80;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      value = c & 15u;
      minimum = 0x800;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      value = c & 7u;
      minimum = 0x10000;
    } else
      return false;
    if (n > size - i)
      return false;
    while (n--) {
      c = p[i++];
      if ((c & 0xc0u) != 0x80u)
        return false;
      value = (value << 6u) | (c & 63u);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}

static bool read_text(pb_istream_t *stream, char *out, size_t capacity) {
  uint64_t size;
  if (!pb_decode_varint(stream, &size) || size >= capacity ||
      size > stream->bytes_left ||
      !pb_read(stream, (pb_byte_t *)out, (size_t)size) ||
      !utf8(out, (size_t)size))
    return false;
  out[size] = 0;
  return true;
}

/* Decode lengths explicitly. Generated static nanopb strings otherwise lose
 * the distinction between an embedded NUL and their C terminator. */
static bool param(pb_istream_t *stream, lua_run_t *run) {
  if (run->value.param_count == H2_GIZCLAW_LUA_APP_PARAMS_MAX)
    return false;
  gizclaw_rpc_v1_ClientLuaAppRunRequest_ParamsEntry entry = {0};
  bool key = false, value = false;
  while (stream->bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(stream, &wire, &tag, &eof) || eof)
      return false;
    if (tag == 1 || tag == 2) {
      bool *seen = tag == 1 ? &key : &value;
      if (*seen || wire != PB_WT_STRING ||
          !read_text(stream, tag == 1 ? entry.key : entry.value,
                     tag == 1 ? sizeof(entry.key) : sizeof(entry.value)))
        return false;
      *seen = true;
    } else if (!pb_skip_field(stream, wire))
      return false;
  }
  if (!key || !entry.key[0])
    return false;
  size_t key_size = strlen(entry.key), value_size = strlen(entry.value);
  if (key_size + value_size >
      H2_GIZCLAW_LUA_APP_PARAMS_BYTES_MAX - run->text_bytes)
    return false;
  for (size_t i = 0; i < run->value.param_count; ++i)
    if (!strcmp(run->value.params[i].name, entry.key))
      return false;
  h2_gizclaw_lua_app_param_t *p = &run->value.params[run->value.param_count++];
  p->name = run->strings + run->used;
  memcpy(run->strings + run->used, entry.key, key_size + 1u);
  run->used += key_size + 1u;
  p->value = run->strings + run->used;
  memcpy(run->strings + run->used, entry.value, value_size + 1u);
  run->used += value_size + 1u;
  run->text_bytes += key_size + value_size;
  return true;
}

static bool run_request(h2_gizclaw_rpc_bytes_t bytes, lua_run_t *run) {
  pb_istream_t stream = pb_istream_from_buffer(bytes.data, bytes.len);
  bool id = false;
  while (stream.bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(&stream, &wire, &tag, &eof) || eof)
      return false;
    if (tag == 1) {
      if (id || wire != PB_WT_STRING ||
          !read_text(&stream, run->value.app.app_id, 33u))
        return false;
      id = true;
    } else if (tag == 2) {
      pb_istream_t sub;
      if (wire != PB_WT_STRING || !pb_make_string_substream(&stream, &sub) ||
          !param(&sub, run) || !pb_close_string_substream(&stream, &sub))
        return false;
    } else if (!pb_skip_field(&stream, wire))
      return false;
  }
  return id && h2_lua_app_id_valid(run->value.app.app_id);
}

typedef struct install_request {
  h2_lua_app_source_t source;
  char sha256[65];
  bool has_sha256;
} install_request_t;

static bool install_request(h2_gizclaw_rpc_bytes_t bytes,
                            install_request_t *request) {
  pb_istream_t stream = pb_istream_from_buffer(bytes.data, bytes.len);
  bool url = false;
  while (stream.bytes_left) {
    pb_wire_type_t wire;
    uint32_t tag;
    bool eof;
    if (!pb_decode_tag(&stream, &wire, &tag, &eof) || eof)
      return false;
    if (tag == 1) {
      uint64_t size;
      if (url || wire != PB_WT_STRING || !pb_decode_varint(&stream, &size) ||
          size > H2_LUA_APP_URL_MAX || size > stream.bytes_left)
        return false;
      const char *data =
          (const char *)bytes.data + bytes.len - stream.bytes_left;
      if (!utf8(data, (size_t)size) || !pb_read(&stream, NULL, (size_t)size))
        return false;
      request->source = (h2_lua_app_source_t){data, (size_t)size};
      url = true;
    } else if (tag == 2) {
      if (request->has_sha256 || wire != PB_WT_STRING ||
          !read_text(&stream, request->sha256, sizeof(request->sha256)))
        return false;
      request->has_sha256 = true;
    } else if (!pb_skip_field(&stream, wire))
      return false;
  }
  if (request->has_sha256 &&
      (strlen(request->sha256) != 64u ||
       strspn(request->sha256, "0123456789abcdefABCDEF") != 64u))
    return false;
  return url;
}

static gizclaw_rpc_v1_LuaAppInfo wire_info(const h2_lua_app_info_t *info) {
  gizclaw_rpc_v1_LuaAppInfo value = {0};
  strcpy(value.app_id, info->app_id);
  strcpy(value.version, info->version);
  value.has_display_name = info->display_name[0] != 0;
  strcpy(value.display_name, info->display_name);
  value.has_description = info->description[0] != 0;
  strcpy(value.description, info->description);
  return value;
}
typedef struct list {
  h2_lua_app_info_t *items;
  size_t count;
} list_t;
static bool list_encode(pb_ostream_t *stream, const pb_field_t *field,
                        void *const *arg) {
  const list_t *list = *arg;
  for (size_t i = 0; i < list->count; ++i) {
    gizclaw_rpc_v1_LuaAppInfo value = wire_info(&list->items[i]);
    if (!pb_encode_tag_for_field(stream, field) ||
        !pb_encode_submessage(stream, gizclaw_rpc_v1_LuaAppInfo_fields, &value))
      return false;
  }
  return true;
}
static int encode(h2_gizclaw_lua_apps_t *apps, const pb_msgdesc_t *fields,
                  const void *value, h2_gizclaw_rpc_provider_response_t *out) {
  size_t size = 0;
  if (!pb_get_encoded_size(&size, fields, value))
    return H2_PAL_ERR_FORMAT;
  if (size > apps->capacity) {
    uint8_t *buffer = h2_pal_mem_alloc(apps->config.allocator, size);
    if (!buffer)
      return H2_PAL_ERR_NO_MEMORY;
    h2_pal_mem_free(apps->config.allocator, apps->response);
    apps->response = buffer;
    apps->capacity = size;
  }
  pb_ostream_t stream = pb_ostream_from_buffer(apps->response, apps->capacity);
  if (!pb_encode(&stream, fields, value))
    return H2_PAL_ERR_FORMAT;
  out->payload = (h2_gizclaw_rpc_bytes_t){apps->response, stream.bytes_written};
  return H2_PAL_OK;
}

static int cancel(void *user) {
  h2_gizclaw_lua_apps_t *apps = user;
  const h2_gizclaw_config_t *c = &apps->config;
  return h2_atomic_load(&apps->stopping) ||
         (c->cancel_requested && c->cancel_requested(c->cancel_user));
}
static void completed(void *user, int result) {
  h2_gizclaw_lua_apps_t *apps = user;
  h2_gizclaw_lua_app_handoff_t handoff = apps->handoff;
  apps->handoff = (h2_gizclaw_lua_app_handoff_t){0};
  if (handoff.complete != NULL)
    handoff.complete(handoff.user, result == H2_PAL_OK && cancel(apps)
                                       ? H2_PAL_ERR_CLOSED
                                       : result);
}
h2_pal_result_t
h2_gizclaw_lua_apps_create_internal(const h2_gizclaw_config_t *config,
                                    h2_gizclaw_lua_apps_t **out) {
  *out = h2_pal_mem_alloc(config->allocator, sizeof(**out));
  if (*out == NULL)
    return H2_PAL_ERR_NO_MEMORY;
  memset(*out, 0, sizeof(**out));
  (*out)->config = *config;
  if (h2_atomic_bool_init(&(*out)->stopping, false) != H2_ATOMIC_OK) {
    h2_pal_mem_free(config->allocator, *out);
    *out = NULL;
    return H2_PAL_ERR_NO_MEMORY;
  }
  return H2_PAL_OK;
}
void h2_gizclaw_lua_apps_cancel_internal(h2_gizclaw_lua_apps_t *apps) {
  if (apps)
    h2_atomic_store(&apps->stopping, true);
}
void h2_gizclaw_lua_apps_destroy_internal(h2_gizclaw_lua_apps_t *apps) {
  if (apps == NULL)
    return;
  completed(apps, H2_PAL_ERR_CLOSED);
  h2_atomic_bool_destroy(&apps->stopping);
  h2_pal_mem_free(apps->config.allocator, apps->response);
  h2_pal_mem_free(apps->config.allocator, apps);
}
int h2_gizclaw_lua_apps_rpc_internal(h2_gizclaw_lua_apps_t *apps,
                                     h2_gizclaw_tool_t tool,
                                     h2_gizclaw_rpc_bytes_t bytes,
                                     h2_gizclaw_rpc_provider_response_t *out) {
  if (apps == NULL)
    return H2_PAL_ERR_UNSUPPORTED;
  if (tool == H2_GIZCLAW_TOOL_LUA_APP_LIST) {
    gizclaw_rpc_v1_ClientLuaAppListRequest request = {0};
    pb_istream_t input = pb_istream_from_buffer(bytes.data, bytes.len);
    if (!pb_decode(&input, gizclaw_rpc_v1_ClientLuaAppListRequest_fields,
                   &request))
      return H2_PAL_ERR_INVALID_ARG;
    list_t list = {0};
    list.items = h2_pal_mem_alloc(apps->config.allocator,
                                  H2_LUA_APPS_MAX * sizeof(*list.items));
    if (!list.items)
      return H2_PAL_ERR_NO_MEMORY;
    int rc = h2_lua_apps_list(apps->config.lua_apps, list.items,
                              H2_LUA_APPS_MAX, &list.count);
    gizclaw_rpc_v1_ClientLuaAppListResponse reply = {
        .apps = {.funcs.encode = list_encode, .arg = &list}};
    if (rc == H2_PAL_OK)
      rc = encode(apps, gizclaw_rpc_v1_ClientLuaAppListResponse_fields, &reply,
                  out);
    h2_pal_mem_free(apps->config.allocator, list.items);
    return rc;
  }
  if (apps->handoff.complete)
    return H2_PAL_ERR_BUSY;
  if (tool == H2_GIZCLAW_TOOL_LUA_APP_INSTALL) {
    install_request_t request = {0};
    if (!install_request(bytes, &request))
      return H2_PAL_ERR_INVALID_ARG;
    // Reserve response storage before committing filesystem state.
    if (apps->capacity < gizclaw_rpc_v1_ClientLuaAppInstallResponse_size) {
      uint8_t *buffer =
          h2_pal_mem_alloc(apps->config.allocator,
                           gizclaw_rpc_v1_ClientLuaAppInstallResponse_size);
      if (buffer == NULL)
        return H2_PAL_ERR_NO_MEMORY;
      h2_pal_mem_free(apps->config.allocator, apps->response);
      apps->response = buffer;
      apps->capacity = gizclaw_rpc_v1_ClientLuaAppInstallResponse_size;
    }
    h2_lua_app_info_t info;
    int rc = h2_lua_apps_install(apps->config.lua_apps, apps->config.http,
                                 request.source,
                                 request.has_sha256 ? request.sha256 : NULL,
                                 110000u, cancel, apps, &info);
    if (rc == H2_PAL_ERR_NO_SPACE)
      return H2_PAL_ERR_UNSUPPORTED;
    if (rc != H2_PAL_OK)
      return rc;
    gizclaw_rpc_v1_ClientLuaAppInstallResponse reply = {
        .has_app = true, .app = wire_info(&info)};
    return encode(apps, gizclaw_rpc_v1_ClientLuaAppInstallResponse_fields,
                  &reply, out);
  }
  if (tool == H2_GIZCLAW_TOOL_LUA_APP_RUN) {
    if (!apps->config.vtable || !apps->config.vtable->prepare_lua_app_run)
      return H2_PAL_ERR_UNSUPPORTED;
    lua_run_t *run = h2_pal_mem_alloc(apps->config.allocator, sizeof(*run));
    if (run == NULL)
      return H2_PAL_ERR_NO_MEMORY;
    memset(run, 0, sizeof(*run));
    int rc = run_request(bytes, run) ? H2_PAL_OK : H2_PAL_ERR_INVALID_ARG;
    char id[33];
    strcpy(id, run->value.app.app_id);
    if (rc == H2_PAL_OK)
      rc = h2_lua_apps_get(apps->config.lua_apps, id, &run->value.app);
    if (rc != H2_PAL_OK) {
      h2_pal_mem_free(apps->config.allocator, run);
      return rc;
    }
    h2_gizclaw_lua_app_handoff_t handoff = {0};
    rc = apps->config.vtable->prepare_lua_app_run(apps->config.user,
                                                  &run->value, &handoff);
    h2_pal_mem_free(apps->config.allocator, run);
    if (rc != H2_PAL_OK)
      return rc;
    if (handoff.complete == NULL)
      return H2_PAL_ERR_INVALID_STATE;
    apps->handoff = handoff;
    out->on_complete = completed;
    out->complete_user = apps;
    return H2_PAL_OK;
  }
  return H2_PAL_ERR_UNSUPPORTED;
}
