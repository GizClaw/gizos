#include "mobile_runner.h"
#include "client.h"
#include <stdio.h>
#include <unistd.h>

int h2_webrtc_mobile_run(h2_runtime_config_t config, const char *offer_url,
                         const char *stun_url,
                         h2_pal_webrtc_e2e_result_t *out) {
  h2_runtime_t *runtime = NULL;
  int rc = h2_runtime_init(&config, &runtime);
  if (rc == H2_PAL_OK) {
    h2_webrtc_fixture_client_t client = {.http = runtime->http,
                                         .offer_url = offer_url};
    const h2_pal_webrtc_e2e_config_t fixture = {
        .runtime = runtime,
        .stun_url = stun_url,
        .exchange_offer = h2_webrtc_fixture_exchange,
        .close_remote = h2_webrtc_fixture_close,
        .fixture_user = &client,
    };
    rc = h2_pal_webrtc_e2e_run(&fixture, out);
    h2_runtime_deinit(runtime);
  }
  return rc;
}
int h2_webrtc_mobile_report(const char *path, const char *platform,
                            const char *version,
                            const h2_pal_webrtc_e2e_result_t *result, int rc,
                            int teardown) {
  FILE *file = fopen(path, "w");
  if (file == NULL)
    return H2_PAL_ERR_IO;
  fprintf(file,
          "{\"platform\":\"%s\",\"version\":\"%s\",\"pid\":%ld,\"contract\":1,"
          "\"operations\":13,\"passed\":%u,\"failed\":%u,\"blocked\":%u,"
          "\"retained_allocations\":%zu,\"rc\":%d,\"teardown\":%d,\"cases\":[",
          platform, version, (long)getpid(), result->passed, result->failed,
          result->blocked, result->retained_allocations, rc, teardown);
  for (unsigned index = 0u; index < H2_PAL_WEBRTC_E2E_CASE_COUNT; ++index) {
    const h2_pal_webrtc_e2e_case_result_t *item = &result->cases[index];
    fprintf(file,
            "%s{\"id\":\"%s\",\"status\":\"%s\",\"detail\":%d,\"line\":%u,"
            "\"elapsed_ms\":%llu,\"observed_error\":%d,\"authentication_evidence\":%d}",
            index == 0u ? "" : ",", item->id != NULL ? item->id : "",
            item->passed    ? "PASS"
            : item->blocked ? "BLOCKED"
                            : "FAIL",
            item->detail, item->line, (unsigned long long)item->elapsed_ms,
            item->observed_error, item->authentication_evidence);
  }
  fputs("]}\n", file);
  if (ferror(file)) {
    fclose(file);
    return H2_PAL_ERR_IO;
  }
  if (fclose(file) != 0)
    return H2_PAL_ERR_IO;
  return rc == H2_PAL_OK && teardown == H2_PAL_OK &&
                 result->passed == H2_PAL_WEBRTC_E2E_CASE_COUNT
             ? H2_PAL_OK
             : H2_PAL_ERR_IO;
}
