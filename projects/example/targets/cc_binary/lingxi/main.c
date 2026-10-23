#include "h2_lingxi.h"
#include "h2_corehttp.h"
#include "h2_desktop_platform.h"
#include "h2_wolfssl.h"
#include "h2_yyjson_json.h"

#if defined(__APPLE__)
#include "h2_darwin_platform.h"
#define host_net h2_darwin_net_api
#define host_entropy h2_darwin_entropy
#else
#include "h2_linux_platform.h"
#define host_net h2_linux_net_api
#define host_entropy h2_linux_entropy
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_CAPACITY 16384u

typedef struct heartbeat_input {
  h2_lingxi_heartbeat_t value;
  char ips[H2_LINGXI_MAX_INNER_IPS][H2_LINGXI_IP_CAPACITY];
  const char* ip_ptrs[H2_LINGXI_MAX_INNER_IPS];
  char fields[8][H2_LINGXI_TEXT_CAPACITY];
} heartbeat_input_t;

static h2_pal_result_t read_file(const char* path, uint8_t* bytes,
                                 size_t capacity, size_t* length) {
  *length = 0u;
  FILE* file = fopen(path, "rb");
  if (file == NULL) return H2_PAL_ERR_IO;
  *length = fread(bytes, 1u, capacity, file);
  const bool valid = !ferror(file) && feof(file) && *length != 0u;
  const int closed = fclose(file);
  return valid && closed == 0 ? H2_PAL_OK : H2_PAL_ERR_FORMAT;
}

static h2_pal_result_t copy_string(const h2_pal_json_api_t* json,
                                   h2_pal_json_value_t* value, char* out,
                                   size_t capacity) {
  h2_pal_json_string_view_t text = {0};
  const h2_pal_result_t rc = h2_pal_json_value_get_string(json, value, &text);
  if (rc != H2_PAL_OK) return rc;
  if (text.len >= capacity || memchr(text.data, '\0', text.len) != NULL)
    return H2_PAL_ERR_FORMAT;
  memcpy(out, text.data, text.len);
  out[text.len] = '\0';
  return H2_PAL_OK;
}

static h2_pal_result_t read_heartbeat(const h2_pal_json_api_t* json,
                                      const char* path,
                                      heartbeat_input_t* input) {
  uint8_t bytes[FILE_CAPACITY];
  size_t length = 0u;
  h2_pal_result_t rc = read_file(path, bytes, sizeof(bytes), &length);
  h2_pal_json_document_t* doc = NULL;
  h2_pal_json_value_t* root = NULL;
  h2_pal_json_value_t* ips = NULL;
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_document_parse(json, bytes, length, NULL, &doc);
  if (rc == H2_PAL_OK) rc = h2_pal_json_document_root(json, doc, &root);
  if (rc == H2_PAL_OK)
    rc = h2_pal_json_object_get(json, root, "innerIp", 7u, &ips);
  size_t count = 0u;
  if (rc == H2_PAL_OK) rc = h2_pal_json_array_size(json, ips, &count);
  if (rc == H2_PAL_OK && (count == 0u || count > H2_LINGXI_MAX_INNER_IPS))
    rc = H2_PAL_ERR_FORMAT;
  for (size_t i = 0u; rc == H2_PAL_OK && i < count; ++i) {
    h2_pal_json_value_t* value = NULL;
    rc = h2_pal_json_array_get(json, ips, i, &value);
    if (rc == H2_PAL_OK)
      rc = copy_string(json, value, input->ips[i], sizeof(input->ips[i]));
    input->ip_ptrs[i] = input->ips[i];
  }
  static const char* const keys[] = {
      "netSpeed",        "netType", "platform", "sdkVersion",
      "firmwareVersion", "imei",    "cmei",     "mac"};
  for (size_t i = 0u; rc == H2_PAL_OK && i < sizeof(keys) / sizeof(keys[0]);
       ++i) {
    h2_pal_json_value_t* value = NULL;
    rc = h2_pal_json_object_get(json, root, keys[i], strlen(keys[i]), &value);
    if (rc == H2_PAL_OK)
      rc = copy_string(json, value, input->fields[i], sizeof(input->fields[i]));
  }
  const h2_pal_result_t destroyed = h2_pal_json_document_destroy(json, &doc);
  if (rc == H2_PAL_OK) rc = destroyed;
  if (rc == H2_PAL_OK)
    input->value = (h2_lingxi_heartbeat_t){
        .inner_ips = input->ip_ptrs,
        .inner_ip_count = count,
        .net_speed = input->fields[0],
        .net_type = input->fields[1],
        .platform = input->fields[2],
        .sdk_version = input->fields[3],
        .firmware_version = input->fields[4],
        .imei = input->fields[5],
        .cmei = input->fields[6],
        .mac = input->fields[7],
    };
  return rc;
}

static void print_result(const char* stage, h2_pal_result_t rc,
                         const h2_lingxi_result_t* result) {
  printf("LINGXI %s result=%d http=%d code=%d success=%d\n", stage, rc,
         result->http_status, result->platform_code,
         result->platform_success ? 1 : 0);
}

int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) {
    fputs(
        "Usage: lingxi BASE_URL DEVICE_NO_TYPE DEVICE_NO [HEARTBEAT_JSON_FILE]\n"
        "Set LINGXI_PRODUCT_ID and LINGXI_PRODUCT_KEY. DEVICE_NO_TYPE: SN/IMEI/CMEI.\n"
        "Optional: LINGXI_SOURCE_IP, LINGXI_INTERFACE, LINGXI_CA_FILE.\n"
        "LINGXI_LOOPBACK_HTTP=1 permits only a local HTTP fixture.\n"
        "An optional JSON file supplies the caller's heartbeat params object.\n",
        stderr);
    return 2;
  }
  h2_lingxi_device_no_type_t type = 0;
  if (strcmp(argv[2], "SN") == 0)
    type = H2_LINGXI_DEVICE_NO_SN;
  else if (strcmp(argv[2], "IMEI") == 0)
    type = H2_LINGXI_DEVICE_NO_IMEI;
  else if (strcmp(argv[2], "CMEI") == 0)
    type = H2_LINGXI_DEVICE_NO_CMEI;
  else
    return 2;
  const h2_lingxi_product_t product = {getenv("LINGXI_PRODUCT_ID"),
                                       getenv("LINGXI_PRODUCT_KEY")};
  if (product.id == NULL || product.key == NULL || product.id[0] == '\0' ||
      product.key[0] == '\0') {
    fputs("Missing Lingxi product credentials\n", stderr);
    return 2;
  }
  const h2_pal_mem_api_t* mem = h2_desktop_platform_default_allocator();
  const h2_wolfssl_config_t tls_config = {.mem = *mem, .entropy = host_entropy};
  h2_pal_result_t rc = h2_wolfssl_init(&tls_config);
  if (rc != H2_PAL_OK) return 2;
  h2_corehttp_t* http_provider = NULL;
  h2_pal_http_api_t http = {0};
  h2_yyjson_json_t* json_provider = NULL;
  h2_lingxi_device_info_t info = {0};
  h2_lingxi_result_t result = {0};
  heartbeat_input_t heartbeat = {0};
  uint8_t ca[FILE_CAPACITY];
  size_t ca_len = 0u;
  const char* ca_path = getenv("LINGXI_CA_FILE");
  if (ca_path != NULL) {
    rc = read_file(ca_path, ca, sizeof(ca), &ca_len);
    if (rc != H2_PAL_OK) goto cleanup;
  }
  const h2_corehttp_config_t http_config = {
      .allocator = mem,
      .net = host_net(),
      .time = h2_desktop_platform_time_api(),
      .tls_verify = H2_PAL_NET_TLS_VERIFY_REQUIRED,
      .root_ca_pem = ca_len != 0u ? ca : NULL,
      .root_ca_pem_len = ca_len,
      .max_header_bytes = 8192u,
      .max_redirects = 0u,
      .default_timeout_ms = 10000u,
      .io_slice_ms = 100u,
  };
  rc = h2_corehttp_create(&http_config, &http_provider, &http);
  if (rc != H2_PAL_OK) goto cleanup;
  rc = h2_yyjson_json_create(mem, &json_provider);
  if (rc != H2_PAL_OK) goto cleanup;
  /* Parse all caller metadata before the first network operation. */
  if (argc == 5) {
    rc = read_heartbeat(h2_yyjson_json_api(json_provider), argv[4], &heartbeat);
    if (rc != H2_PAL_OK) goto cleanup;
  }
  const char* loopback = getenv("LINGXI_LOOPBACK_HTTP");
  const h2_lingxi_config_t config = {
      .allocator = mem,
      .http = &http,
      .json = h2_yyjson_json_api(json_provider),
      .base_url = argv[1],
      .source_ip = getenv("LINGXI_SOURCE_IP"),
      .interface_name = getenv("LINGXI_INTERFACE"),
      .json_limits = {FILE_CAPACITY, 24u, 512u},
      .timeout_ms = 10000u,
      .allow_plaintext_loopback =
          loopback != NULL && strcmp(loopback, "1") == 0,
  };
  const h2_lingxi_identity_t identity = {type, argv[3]};
  rc = h2_lingxi_get_device_info(&config, &product, &identity, &info, &result);
  print_result("get-device-info", rc, &result);
  if (rc == H2_PAL_OK && argc == 5) {
    rc = h2_lingxi_report_heartbeat(&config, &product, &info, &heartbeat.value,
                                    &result);
    print_result("heartbeat", rc, &result);
  }
cleanup:
  h2_lingxi_device_info_clear(&info);
  if (json_provider != NULL) {
    const h2_pal_result_t destroyed = h2_yyjson_json_destroy(&json_provider);
    if (rc == H2_PAL_OK) rc = destroyed;
  }
  h2_corehttp_destroy(http_provider);
  const h2_pal_result_t destroyed = h2_wolfssl_deinit();
  if (rc == H2_PAL_OK) rc = destroyed;
  printf("LINGXI finished result=%d\n", rc);
  return rc == H2_PAL_OK ? 0 : 1;
}
