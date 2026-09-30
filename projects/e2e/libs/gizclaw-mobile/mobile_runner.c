#include "mobile_runner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mobile_run {
  h2_runtime_t *runtime;
  h2_gizclaw_e2e_config_t config;
  char endpoint[128], token[4097], api[1024], audio[2048];
  uint8_t *pcm;
} mobile_run_t;
static mobile_run_t *retained;

static bool copy(char *to, size_t capacity, const char *from) {
  if (!from || !from[0] || strlen(from) >= capacity) return false;
  strcpy(to, from); return true;
}
int h2_gizclaw_mobile_run(h2_runtime_config_t runtime_config,
    const char *platform, const char *endpoint, const char *token,
    const char *api_url, const char *audio_url, const uint8_t *pcm, size_t pcm_len,
    h2_gizclaw_e2e_result_t *result) {
  if ((!h2_gizclaw_e2e_fixture_key()[0] ||
       !h2_gizclaw_e2e_fixture_profile()[0] || !h2_gizclaw_e2e_fixture_value()[0])) return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  if (!result || retained || !platform || !pcm || !pcm_len ||
      (pcm_len & 1u) || pcm_len > 1024u * 1024u) return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  *result = (h2_gizclaw_e2e_result_t){0};
  mobile_run_t *run = calloc(1u, sizeof(*run));
  if (!run) return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  int rc = H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  if (!copy(run->endpoint, sizeof(run->endpoint), endpoint) ||
      !copy(run->token, sizeof(run->token), token) ||
      !copy(run->api, sizeof(run->api), api_url) ||
      !copy(run->audio, sizeof(run->audio), audio_url)) goto done;
  for (const char *p = run->endpoint; *p; ++p)
    if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == ':')) goto done;
  run->pcm = malloc(pcm_len);
  if (!run->pcm) goto done;
  memcpy(run->pcm, pcm, pcm_len);
  run->config = (h2_gizclaw_e2e_config_t){
      .server_endpoint={run->endpoint,strlen(run->endpoint)},
      .registration_token={run->token,strlen(run->token)},
      .app_config_key=h2_gizclaw_e2e_fixture_key(),
      .expected_runtime_profile=h2_gizclaw_e2e_fixture_profile(),
      .app_config_expected_value={h2_gizclaw_e2e_fixture_value(),
                                 strlen(h2_gizclaw_e2e_fixture_value())},
      .device_api_url=run->api, .device_audio_url=run->audio,
      .voice_pcm_s16le_16khz_mono=run->pcm, .voice_pcm_len=pcm_len,
      .suites=H2_GIZCLAW_E2E_SUITE_ALL};
  if (h2_runtime_init(&runtime_config, &run->runtime) != H2_PAL_OK) goto done;
  rc = h2_gizclaw_e2e_run(run->runtime, &run->config, result);
  printf("H2_GIZCLAW_E2E stage=summary platform=%s endpoint=%s backend=h2peer "
      "suite=all profile=%s selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu "
      "blocked=%zu cancelled=%zu first_failure_case=%s first_failure_rc=%d "
      "cleanup_rc=%d retained_resources=%zu complete=%s exit_code=%d\n",
      platform, run->endpoint, result->runtime_profile_name[0] ? result->runtime_profile_name : "-",
      result->selected, result->terminal, result->passed, result->failed, result->errors,
      result->blocked, result->cancelled, result->first_failure_case[0] ? result->first_failure_case : "-",
      result->first_failure_rc, result->cleanup_rc, result->retained_resources,
      result->complete ? "true" : "false", rc);
  fflush(stdout);
  if (result->retained_resources) { retained = run; return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR; }
  h2_runtime_deinit(run->runtime);
done:
  memset(run->token, 0, sizeof(run->token));
  free(run->pcm); free(run); return rc;
}
int h2_gizclaw_mobile_report(const char *path, const char *platform,
    const h2_gizclaw_e2e_result_t *r, int rc, int teardown) {
  if (!path || !platform || !r) return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  FILE *f = fopen(path,"w"); if (!f) return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  fprintf(f,"{\"platform\":\"%s\",\"rc\":%d,\"teardown\":%d,\"selected\":%zu,"
      "\"passed\":%zu,\"failed\":%zu,\"cleanup_rc\":%d,\"retained_resources\":%zu,\"complete\":%s}\n",
      platform,rc,teardown,r->selected,r->passed,r->failed,r->cleanup_rc,r->retained_resources,r->complete ? "true":"false");
  bool failed=ferror(f)!=0; if(fclose(f))failed=true;
  return failed || teardown || r->retained_resources ? H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR : rc;
}
