#include "h2_alipay_client_internal.h"

#include "yyjson.h"
#include <string.h>

static int discard_account(const h2_alipay_client_config_t* config) {
  h2_pal_pref_namespace_t* namespace = NULL;
  int rc = h2_pal_pref_open(config->pref, config->pref_namespace,
                            H2_PAL_PREF_OPEN_READ_WRITE, &namespace);
  if (rc != H2_PAL_OK) return rc < 0 ? rc : H2_PAL_ERR_IO;
  if (namespace == NULL) return H2_PAL_ERR_INVALID_STATE;
  if (namespace->remove == NULL || namespace->commit == NULL)
    return h2_alipay_client_finish_namespace(namespace, H2_PAL_ERR_UNSUPPORTED);
  rc = namespace->remove(namespace, "binding");
  if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  if (rc == H2_PAL_OK) {
    rc = namespace->remove(namespace, "context");
    if (rc == H2_PAL_ERR_NOT_FOUND) rc = H2_PAL_OK;
  }
  if (rc == H2_PAL_OK) rc = namespace->commit(namespace);
  return h2_alipay_client_finish_namespace(namespace, rc);
}

static void notify_remote(const h2_alipay_client_config_t* config,
                          client_workspace_t* workspace,
                          const binding_record_t* binding,
                          h2_alipay_client_result_t* result) {
  const yyjson_alc allocator = {h2_alipay_client_json_alloc,
                                h2_alipay_client_json_realloc,
                                h2_alipay_client_json_free, (void*)config->mem};
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  if (document == NULL) return;
  yyjson_mut_val* array = yyjson_mut_arr(document);
  yyjson_mut_val* object = yyjson_mut_obj(document);
  bool ok = array != NULL && object != NULL &&
            yyjson_mut_obj_add_str(document, object, "tid", binding->tid) &&
            yyjson_mut_obj_add_str(document, object, "userid", binding->uid) &&
            yyjson_mut_arr_add_val(array, object);
  yyjson_mut_doc_set_root(document, array);
  size_t length = 0u;
  char* body =
      ok ? yyjson_mut_write_opts(document, 0u, &allocator, &length, NULL)
         : NULL;
  if (body != NULL) {
    size_t response_length = 0u;
    (void)h2_alipay_client_post(config, workspace, result,
                                "alipay.mobile.alideviceinfo.changecode.unbind",
                                false, (const uint8_t*)body, length,
                                &response_length);
    h2_alipay_client_json_free((void*)config->mem, body);
  }
  yyjson_mut_doc_free(document);
}

h2_pal_result_t h2_alipay_client_unbind(const h2_alipay_client_config_t* config,
                                        h2_alipay_client_result_t* out) {
  if (out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  out->rpc_status = -1;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  binding_record_t* binding = h2_pal_mem_alloc(config->mem, sizeof(*binding));
  client_workspace_t* workspace =
      h2_pal_mem_alloc(config->mem, sizeof(*workspace));
  if (binding != NULL) memset(binding, 0, sizeof(*binding));
  if (workspace != NULL) memset(workspace, 0, sizeof(*workspace));
  out->stage = H2_ALIPAY_CLIENT_UNBIND;
  if (binding != NULL && workspace != NULL &&
      h2_alipay_client_load_binding(config, binding) == H2_PAL_OK &&
      h2_alipay_client_binding_complete(binding) &&
      h2_alipay_client_load_state(config, &workspace->state) == H2_PAL_OK)
    notify_remote(config, workspace, binding, out);
  out->stage = H2_ALIPAY_CLIENT_STORAGE;
  rc = discard_account(config);
  if (binding != NULL) h2_alipay_client_wipe(binding, sizeof(*binding));
  if (workspace != NULL) h2_alipay_client_wipe(workspace, sizeof(*workspace));
  h2_pal_mem_free(config->mem, binding);
  h2_pal_mem_free(config->mem, workspace);
  return rc;
}

static int remote_request(const h2_alipay_client_config_t* config,
                          client_workspace_t* workspace,
                          const binding_record_t* binding,
                          h2_alipay_client_result_t* result, bool balance,
                          size_t* length) {
  const yyjson_alc allocator = {h2_alipay_client_json_alloc,
                                h2_alipay_client_json_realloc,
                                h2_alipay_client_json_free, (void*)config->mem};
  yyjson_mut_doc* document = yyjson_mut_doc_new(&allocator);
  if (document == NULL) return H2_PAL_ERR_NO_MEMORY;
  yyjson_mut_val* array = yyjson_mut_arr(document);
  yyjson_mut_val* object = yyjson_mut_obj(document);
  yyjson_mut_val* businesses = yyjson_mut_arr(document);
  bool ok = array != NULL && object != NULL && businesses != NULL &&
            (!balance ||
             yyjson_mut_obj_add_str(document, object, "source", "WATCH_SDK")) &&
            yyjson_mut_obj_add_str(document, object, "tid", binding->tid) &&
            yyjson_mut_obj_add_str(document, object, "userid", binding->uid) &&
            yyjson_mut_obj_add_str(document, object, "version", "4.61") &&
            yyjson_mut_arr_add_str(document, businesses, "BALANCE") &&
            yyjson_mut_obj_add_val(document, object, "queryBizTypeList",
                                   businesses) &&
            yyjson_mut_arr_add_val(array, object);
  yyjson_mut_doc_set_root(document, array);
  size_t size = 0u;
  char* body =
      ok ? yyjson_mut_write_opts(document, 0u, &allocator, &size, NULL) : NULL;
  int rc = H2_PAL_ERR_NO_MEMORY;
  if (body != NULL)
    rc = h2_alipay_client_post(
        config, workspace, result,
        "alipay.mobile.alideviceinfo.changecode.device.queryDeviceInfo", false,
        (const uint8_t*)body, size, length);
  h2_alipay_client_json_free((void*)config->mem, body);
  yyjson_mut_doc_free(document);
  return rc;
}

static int read_response(const h2_alipay_client_config_t* config,
                         client_workspace_t* workspace, size_t length,
                         yyjson_doc** document) {
  const yyjson_alc allocator = {h2_alipay_client_json_alloc,
                                h2_alipay_client_json_realloc,
                                h2_alipay_client_json_free, (void*)config->mem};
  yyjson_read_err error;
  *document = yyjson_read_opts((char*)workspace->response, length, 0u,
                               &allocator, &error);
  if (*document == NULL)
    return error.code == YYJSON_READ_ERROR_MEMORY_ALLOCATION
               ? H2_PAL_ERR_NO_MEMORY
               : H2_PAL_ERR_FORMAT;
  yyjson_val* root = yyjson_doc_get_root(*document);
  if (!yyjson_is_obj(root) || yyjson_obj_size(root) > 64u)
    return H2_PAL_ERR_FORMAT;
  yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
  yyjson_val* key;
  while ((key = yyjson_obj_iter_next(&iterator)) != NULL) {
    const char* name = yyjson_get_str(key);
    if (strlen(name) != yyjson_get_len(key) ||
        yyjson_obj_get(root, name) != yyjson_obj_iter_get_val(key))
      return H2_PAL_ERR_FORMAT;
  }
  return H2_PAL_OK;
}

static int remote_response(const h2_alipay_client_config_t* config,
                           client_workspace_t* workspace, size_t length,
                           h2_alipay_client_remote_binding_t* state) {
  yyjson_doc* document = NULL;
  int rc = read_response(config, workspace, length, &document);
  if (rc != H2_PAL_OK) goto done;
  yyjson_val* root = yyjson_doc_get_root(document);
  yyjson_val* binding = yyjson_obj_get(root, "watchBindStatus");
  if (yyjson_is_str(binding)) {
    *state = yyjson_equals_str(binding, "BINDED")
                 ? H2_ALIPAY_CLIENT_REMOTE_BOUND
             : yyjson_equals_str(binding, "UNBIND")
                 ? H2_ALIPAY_CLIENT_REMOTE_UNBOUND
                 : H2_ALIPAY_CLIENT_REMOTE_UNKNOWN;
  }
  yyjson_val* code = yyjson_obj_get(root, "errorCode");
  if (yyjson_is_str(code))
    *state = yyjson_equals_str(code, "1007") ? H2_ALIPAY_CLIENT_REMOTE_UNBOUND
                                             : H2_ALIPAY_CLIENT_REMOTE_UNKNOWN;
  rc = H2_PAL_OK;
done:
  yyjson_doc_free(document);
  return rc;
}

h2_pal_result_t h2_alipay_client_query_remote_binding(
    const h2_alipay_client_config_t* config,
    h2_alipay_client_remote_binding_t* state, h2_alipay_client_result_t* out) {
  if (state == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  *state = H2_ALIPAY_CLIENT_REMOTE_UNKNOWN;
  memset(out, 0, sizeof(*out));
  out->rpc_status = -1;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  binding_record_t* binding = h2_pal_mem_alloc(config->mem, sizeof(*binding));
  client_workspace_t* workspace =
      h2_pal_mem_alloc(config->mem, sizeof(*workspace));
  if (binding != NULL) memset(binding, 0, sizeof(*binding));
  if (workspace != NULL) memset(workspace, 0, sizeof(*workspace));
  if (binding == NULL || workspace == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  out->stage = H2_ALIPAY_CLIENT_STORAGE;
  rc = h2_alipay_client_load_binding(config, binding);
  if (rc == H2_PAL_ERR_NOT_FOUND ||
      (rc == H2_PAL_OK && !h2_alipay_client_binding_complete(binding))) {
    *state = H2_ALIPAY_CLIENT_REMOTE_UNBOUND;
    rc = H2_PAL_OK;
    goto done;
  }
  if (rc != H2_PAL_OK) goto done;
  *state = H2_ALIPAY_CLIENT_REMOTE_BOUND;
  rc = h2_alipay_client_load_state(config, &workspace->state);
  if (rc != H2_PAL_OK) goto done;
  out->stage = H2_ALIPAY_CLIENT_QUERY_REMOTE_BINDING;
  size_t length = 0u;
  rc = remote_request(config, workspace, binding, out, false, &length);
  if (rc == H2_PAL_OK) rc = remote_response(config, workspace, length, state);
  if (rc == H2_PAL_OK && *state == H2_ALIPAY_CLIENT_REMOTE_UNBOUND) {
    h2_alipay_client_result_t cleanup;
    rc = h2_alipay_client_unbind(config, &cleanup);
    out->requests += cleanup.requests;
    h2_alipay_client_result_clear(&cleanup);
  }
done:
  if (binding != NULL) h2_alipay_client_wipe(binding, sizeof(*binding));
  if (workspace != NULL) h2_alipay_client_wipe(workspace, sizeof(*workspace));
  h2_pal_mem_free(config->mem, binding);
  h2_pal_mem_free(config->mem, workspace);
  return rc;
}

static int balance_response(const h2_alipay_client_config_t* config,
                            client_workspace_t* workspace, size_t length,
                            char bitmap[32][14]) {
  yyjson_doc* document = NULL;
  int rc = read_response(config, workspace, length, &document);
  if (rc != H2_PAL_OK) goto done;
  yyjson_val* root = yyjson_doc_get_root(document);
  yyjson_val* data = yyjson_obj_get(root, "changeCodeVo");
  yyjson_val* amount = yyjson_obj_get(data, "balance");
  if (!yyjson_is_true(yyjson_obj_get(root, "success")) ||
      !yyjson_is_obj(data) || !yyjson_is_str(amount) ||
      yyjson_get_len(amount) >= 20u ||
      strlen(yyjson_get_str(amount)) != yyjson_get_len(amount)) {
    rc = H2_PAL_ERR_FORMAT;
    goto done;
  }
  memset(bitmap, 0, 4u);
  h2_alipay_amount_bitmap(yyjson_get_str(amount), bitmap);
done:
  yyjson_doc_free(document);
  return rc;
}

h2_pal_result_t h2_alipay_client_get_balance_bitmap(
    const h2_alipay_client_config_t* config, char bitmap[32][14],
    h2_alipay_client_result_t* out) {
  if (bitmap == NULL || out == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out, 0, sizeof(*out));
  out->rpc_status = -1;
  int rc = h2_alipay_client_validate_config(config);
  if (rc != H2_PAL_OK) return rc;
  binding_record_t* binding = h2_pal_mem_alloc(config->mem, sizeof(*binding));
  client_workspace_t* workspace =
      h2_pal_mem_alloc(config->mem, sizeof(*workspace));
  if (binding != NULL) memset(binding, 0, sizeof(*binding));
  if (workspace != NULL) memset(workspace, 0, sizeof(*workspace));
  if (binding == NULL || workspace == NULL) {
    rc = H2_PAL_ERR_NO_MEMORY;
    goto done;
  }
  out->stage = H2_ALIPAY_CLIENT_STORAGE;
  rc = h2_alipay_client_load_binding(config, binding);
  if (rc != H2_PAL_OK) goto done;
  if (!h2_alipay_client_binding_complete(binding)) {
    rc = H2_PAL_ERR_NOT_FOUND;
    goto done;
  }
  rc = h2_alipay_client_load_state(config, &workspace->state);
  if (rc != H2_PAL_OK) goto done;
  out->stage = H2_ALIPAY_CLIENT_QUERY_BALANCE;
  size_t length = 0u;
  rc = remote_request(config, workspace, binding, out, true, &length);
  if (rc == H2_PAL_OK) rc = balance_response(config, workspace, length, bitmap);
done:
  if (binding != NULL) h2_alipay_client_wipe(binding, sizeof(*binding));
  if (workspace != NULL) h2_alipay_client_wipe(workspace, sizeof(*workspace));
  h2_pal_mem_free(config->mem, binding);
  h2_pal_mem_free(config->mem, workspace);
  return rc;
}
