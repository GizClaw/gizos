#include "h2_lingxi_e2e.h"

#include <string.h>

static const char* const case_ids[] = {
#define H2_LINGXI_CASE(symbol, id) id,
#include "h2_lingxi_cases.inc"
#undef H2_LINGXI_CASE
};

static void publish(const h2_lingxi_e2e_config_t* config,
                    h2_lingxi_e2e_result_t* result, unsigned index) {
  const h2_lingxi_e2e_case_result_t* row = &result->cases[index];
  if (row->status == H2_LINGXI_E2E_PASS)
    ++result->passed;
  else if (row->status == H2_LINGXI_E2E_FAIL)
    ++result->failed;
  else
    ++result->blocked;
  if (config != NULL && config->report != NULL)
    config->report(config->report_user, row);
}

static bool ready(const h2_lingxi_e2e_config_t* config) {
  return config != NULL && config->time != NULL &&
         config->time->vtable != NULL &&
         config->time->vtable->get_monotonic_ms != NULL &&
         config->client.allocator != NULL && config->client.http != NULL &&
         config->client.json != NULL && config->client.base_url != NULL &&
         config->client.timeout_ms != 0u && config->product.id != NULL &&
         config->product.key != NULL && config->identity.number != NULL &&
         config->heartbeat.inner_ips != NULL &&
         config->heartbeat.inner_ip_count != 0u &&
         config->heartbeat.net_speed != NULL &&
         config->heartbeat.net_type != NULL &&
         config->heartbeat.platform != NULL &&
         config->heartbeat.sdk_version != NULL &&
         config->heartbeat.firmware_version != NULL;
}

static void finish_request(const h2_lingxi_e2e_config_t* config,
                           h2_lingxi_e2e_case_result_t* row, uint64_t start,
                           h2_pal_result_t rc) {
  uint64_t end = 0u;
  h2_pal_result_t timed = h2_pal_time_get_monotonic_ms(config->time, &end);
  if (timed == H2_PAL_OK && end < start) timed = H2_PAL_ERR_INVALID_STATE;
  if (timed == H2_PAL_OK) row->elapsed_ms = end - start;
  if (rc == H2_PAL_OK) rc = timed;
  if (rc == H2_PAL_OK && row->elapsed_ms > config->client.timeout_ms)
    rc = H2_PAL_ERR_TIMEOUT;
  row->result = rc;
  row->status = rc == H2_PAL_OK ? H2_LINGXI_E2E_PASS : H2_LINGXI_E2E_FAIL;
}

h2_pal_result_t h2_lingxi_e2e_run(const h2_lingxi_e2e_config_t* config,
                                  h2_lingxi_e2e_result_t* out_result) {
  if (out_result == NULL) return H2_PAL_ERR_INVALID_ARG;
  memset(out_result, 0, sizeof(*out_result));
  for (unsigned i = 0u; i < H2_LINGXI_E2E_CASE_COUNT; ++i) {
    out_result->cases[i].id = case_ids[i];
    out_result->cases[i].result = H2_PAL_ERR_INVALID_STATE;
  }
  if (!ready(config)) {
    for (unsigned i = 0u; i < H2_LINGXI_E2E_CASE_COUNT; ++i) {
      out_result->cases[i].result = H2_PAL_ERR_INVALID_ARG;
      publish(config, out_result, i);
    }
    return H2_PAL_ERR_INVALID_ARG;
  }
  h2_lingxi_device_info_t info = {0};
  h2_lingxi_e2e_case_result_t* row =
      &out_result->cases[H2_LINGXI_E2E_GET_DEVICE_INFO];
  uint64_t start = 0u;
  h2_pal_result_t rc = h2_pal_time_get_monotonic_ms(config->time, &start);
  if (rc == H2_PAL_OK)
    rc = h2_lingxi_get_device_info(&config->client, &config->product,
                                   &config->identity, &info, &row->platform);
  finish_request(config, row, start, rc);
  publish(config, out_result, H2_LINGXI_E2E_GET_DEVICE_INFO);
  const h2_pal_result_t query_rc = row->result;

  row = &out_result->cases[H2_LINGXI_E2E_HEARTBEAT];
  if (query_rc == H2_PAL_OK) {
    rc = h2_pal_time_get_monotonic_ms(config->time, &start);
    if (rc == H2_PAL_OK)
      rc = h2_lingxi_report_heartbeat(&config->client, &config->product, &info,
                                      &config->heartbeat, &row->platform);
    finish_request(config, row, start, rc);
  }
  publish(config, out_result, H2_LINGXI_E2E_HEARTBEAT);
  const h2_pal_result_t heartbeat_rc = row->result;

  h2_lingxi_device_info_clear(&info);
  row = &out_result->cases[H2_LINGXI_E2E_CREDENTIAL_CLEANUP];
  row->result = H2_PAL_OK;
  const unsigned char* bytes = (const unsigned char*)&info;
  for (size_t i = 0u; i < sizeof(info); ++i)
    if (bytes[i] != 0u) row->result = H2_PAL_ERR_INVALID_STATE;
  row->status =
      row->result == H2_PAL_OK ? H2_LINGXI_E2E_PASS : H2_LINGXI_E2E_FAIL;
  publish(config, out_result, H2_LINGXI_E2E_CREDENTIAL_CLEANUP);
  return query_rc != H2_PAL_OK       ? query_rc
         : heartbeat_rc != H2_PAL_OK ? heartbeat_rc
                                     : row->result;
}
