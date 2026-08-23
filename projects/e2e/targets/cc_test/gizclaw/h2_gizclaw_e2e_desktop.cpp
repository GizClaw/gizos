#include "h2_gizclaw_e2e_desktop.h"

#include "h2_corehttp.h"
#include "h2_desktop_app_support.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_pal_e2e_access_point.h"
#ifdef H2_GIZCLAW_E2E_USE_PION
#include "h2_pion.h"
#endif

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop_requested;

void request_stop(int) { g_stop_requested = 1; }

bool should_stop(void *) { return g_stop_requested != 0; }

uint32_t parse_suite(const char *value) {
  if (value == nullptr) {
    return 0u;
  }
  if (std::strcmp(value, "all") == 0) {
    return H2_GIZCLAW_E2E_SUITE_ALL;
  }
  if (std::strcmp(value, "connectivity") == 0) {
    return H2_GIZCLAW_E2E_SUITE_CONNECTIVITY;
  }
  if (std::strcmp(value, "rpc") == 0) {
    return H2_GIZCLAW_E2E_SUITE_RPC;
  }
  if (std::strcmp(value, "firmware") == 0) {
    return H2_GIZCLAW_E2E_SUITE_FIRMWARE;
  }
  if (std::strcmp(value, "voice") == 0) {
    return H2_GIZCLAW_E2E_SUITE_VOICE;
  }
  if (std::strcmp(value, "firmware-voice") == 0) {
    return H2_GIZCLAW_E2E_SUITE_FIRMWARE | H2_GIZCLAW_E2E_SUITE_VOICE;
  }
  if (std::strcmp(value, "concurrency") == 0) {
    return H2_GIZCLAW_E2E_SUITE_CONCURRENCY;
  }
  if (std::strcmp(value, "service") == 0) {
    return H2_GIZCLAW_E2E_SUITE_SERVICE;
  }
  return 0u;
}

void emit_progress(void *, const h2_gizclaw_e2e_progress_t *progress) {
  if (progress == nullptr) {
    return;
  }
  std::printf(
      "H2_GIZCLAW_E2E kind=%u case=%s status=%s rc=%d "
      "selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu blocked=%zu "
      "cancelled=%zu elapsed_ms=%llu complete=%s\n",
      static_cast<unsigned>(progress->kind),
      progress->case_id == nullptr ? "-" : progress->case_id,
      progress->status == nullptr ? "-" : progress->status, progress->rc,
      progress->selected, progress->terminal, progress->passed,
      progress->failed, progress->errors, progress->blocked,
      progress->cancelled,
      static_cast<unsigned long long>(progress->elapsed_ms),
      progress->complete ? "true" : "false");
  std::fflush(stdout);
}

bool load_pcm(const char *path, std::vector<uint8_t> *out_pcm) {
  if (path == nullptr || path[0] == '\0' || out_pcm == nullptr) {
    return false;
  }
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return false;
  }
  const std::streamsize size = input.tellg();
  if (size <= 0 || size > 1024 * 1024 || (size % 2) != 0) {
    return false;
  }
  input.seekg(0, std::ios::beg);
  out_pcm->resize(static_cast<size_t>(size));
  if (!input.read(reinterpret_cast<char *>(out_pcm->data()), size)) {
    out_pcm->clear();
    return false;
  }
  bool non_silent = false;
  for (uint8_t byte : *out_pcm) {
    non_silent = non_silent || byte != 0u;
  }
  return non_silent;
}

int run_desktop(int argc, char **argv) {
  if (argc != 4 || argv == nullptr || argv[1] == nullptr ||
      argv[2] == nullptr || argv[3] == nullptr) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=missing-input\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  const char *token_value = std::getenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN");
  const size_t token_len =
      token_value == nullptr ? 0u : std::strlen(token_value);
  const char *suite_value = std::getenv("H2_GIZCLAW_E2E_SUITE");
  if (suite_value == nullptr || suite_value[0] == '\0') {
    suite_value = argv[2];
  }
  const char *entry_value = std::getenv("H2_GIZCLAW_E2E_ENTRY");
  if (entry_value == nullptr || entry_value[0] == '\0') {
    entry_value = argv[3];
  }
  const uint32_t suites = parse_suite(suite_value);
  h2_gizclaw_pal_e2e_access_point_t access_point;
  const int entry_rc =
      h2_gizclaw_pal_e2e_access_point_parse(entry_value, &access_point);
#ifdef H2_GIZCLAW_E2E_USE_PION
  const uint32_t supported_suites =
      H2_GIZCLAW_E2E_SUITE_RPC | H2_GIZCLAW_E2E_SUITE_FIRMWARE |
      H2_GIZCLAW_E2E_SUITE_VOICE;
  const bool suite_supported =
      (suites & ~supported_suites) == 0u && suites != 0u;
#else
  const bool suite_supported = true;
#endif
  if (token_len == 0u || token_len > H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX ||
      suites == 0u || !suite_supported || entry_rc != H2_PAL_OK) {
    const char *reason =
        token_len == 0u ? "missing-token"
                        : (token_len > H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX
                               ? "oversized-token"
                               : (suites == 0u ? "invalid-suite"
                                               : (!suite_supported
                                                      ? "unsupported-pion-suite"
                                                      : "invalid-entry")));
    std::fprintf(stderr,
                 "H2_GIZCLAW_E2E stage=preflight status=ERROR reason=%s\n",
                 reason);
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  const char *endpoint = h2_gizclaw_pal_e2e_access_point_endpoint(access_point);
  if (endpoint == nullptr) {
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }

  std::vector<uint8_t> pcm;
  if ((suites & (H2_GIZCLAW_E2E_SUITE_RPC | H2_GIZCLAW_E2E_SUITE_VOICE)) !=
          0u &&
      !load_pcm(argv[1], &pcm)) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=invalid-voice-input\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }

  std::string token(token_value, token_len);
  h2::desktop::OwnedNetworkServices network;
#ifdef H2_GIZCLAW_E2E_USE_PION
  int rc = h2::desktop::open_network_services(false, false, &network);
#else
  int rc = h2::desktop::open_network_services(false, true, &network);
#endif
  h2_corehttp_t *http_provider = nullptr;
  h2_pal_http_api_t http_api = {};
  const h2_corehttp_config_t http_config = {
      .allocator = h2_desktop_platform_default_allocator(),
      .net = h2::desktop::host_net_api(),
      .time = h2_desktop_platform_time_api(),
      .log = h2_desktop_platform_log_api(),
      .tls_verify = H2_PAL_NET_TLS_VERIFY_DEFAULT,
      .root_ca_pem = nullptr,
      .root_ca_pem_len = 0u,
      .max_header_bytes = 0u,
      .max_redirects = 0u,
      .default_timeout_ms = 0u,
      .io_slice_ms = 0u,
  };
  if (rc == H2_PAL_OK) {
    rc = h2_corehttp_create(&http_config, &http_provider, &http_api);
  }

  const h2_pal_webrtc_api_t *webrtc = nullptr;
#ifdef H2_GIZCLAW_E2E_USE_PION
  h2_pion_t *pion = nullptr;
  const h2_pion_config_t pion_config = {
      .mem = h2_desktop_platform_default_allocator(),
  };
  if (rc == H2_PAL_OK) {
    rc = h2_pion_create(&pion_config, &pion);
  }
  if (rc == H2_PAL_OK) {
    webrtc = h2_pion_webrtc_api(pion);
    if (webrtc == nullptr) {
      rc = H2_PAL_ERR_UNAVAILABLE;
    }
  }
#else
  if (rc == H2_PAL_OK) {
    webrtc = network.webrtc();
    if (webrtc == nullptr) {
      rc = H2_PAL_ERR_UNSUPPORTED;
    }
  }
#endif

  h2_runtime_t *runtime = nullptr;
  if (rc == H2_PAL_OK) {
    h2_runtime_config_t runtime_config = h2::desktop::runtime_config(nullptr);
    runtime_config.crypto = network.crypto();
    runtime_config.http = &http_api;
    runtime_config.net = h2::desktop::host_net_api();
    runtime_config.webrtc = webrtc;
    rc = h2_runtime_init(&runtime_config, &runtime);
  }
  if (rc != H2_PAL_OK || runtime == nullptr) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=provider status=ERROR rc=%d\n",
                 rc);
    if (runtime != nullptr) {
      h2_runtime_deinit(runtime);
    }
#ifdef H2_GIZCLAW_E2E_USE_PION
    h2_pion_destroy(&pion);
#endif
    h2_corehttp_destroy(http_provider);
    std::fill(token.begin(), token.end(), '\0');
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }

  g_stop_requested = 0;
  (void)std::signal(SIGINT, request_stop);
  (void)std::signal(SIGTERM, request_stop);
  const h2_gizclaw_e2e_config_t app_config = {
      .server_endpoint = {endpoint, std::strlen(endpoint)},
      .registration_token = {token.data(), token.size()},
      .voice_pcm_s16le_16khz_mono = pcm.empty() ? nullptr : pcm.data(),
      .voice_pcm_len = pcm.size(),
      .suites = suites,
      .case_timeout_ms = H2_GIZCLAW_E2E_DEFAULT_CASE_TIMEOUT_MS,
      .cleanup_timeout_ms = H2_GIZCLAW_E2E_DEFAULT_CLEANUP_TIMEOUT_MS,
      .progress_interval_ms = H2_GIZCLAW_E2E_DEFAULT_PROGRESS_INTERVAL_MS,
      .should_stop = should_stop,
      .should_stop_user = nullptr,
      .on_progress = emit_progress,
      .progress_user = nullptr,
  };
  h2_gizclaw_e2e_result_t result = {};
  const h2_gizclaw_e2e_exit_t exit_code =
      h2_gizclaw_e2e_run(runtime, &app_config, &result);
  std::printf(
      "H2_GIZCLAW_E2E stage=summary entry=%s backend=%s suite=%s profile=%s "
      "selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu blocked=%zu "
      "cancelled=%zu first_failure_case=%s first_failure_rc=%d "
      "cleanup_rc=%d retained_resources=%zu complete=%s exit_code=%d\n",
      h2_gizclaw_pal_e2e_access_point_name(access_point),
#ifdef H2_GIZCLAW_E2E_USE_PION
      "pion",
#else
      "h2peer",
#endif
      suite_value,
      result.runtime_profile_name[0] == '\0' ? "-"
                                             : result.runtime_profile_name,
      result.selected, result.terminal, result.passed, result.failed,
      result.errors, result.blocked, result.cancelled,
      result.first_failure_case[0] == '\0' ? "-" : result.first_failure_case,
      result.first_failure_rc, result.cleanup_rc, result.retained_resources,
      result.complete ? "true" : "false", static_cast<int>(exit_code));
  std::fflush(stdout);

  h2_runtime_deinit(runtime);
#ifdef H2_GIZCLAW_E2E_USE_PION
  h2_pion_destroy(&pion);
#endif
  h2_corehttp_destroy(http_provider);
  std::fill(token.begin(), token.end(), '\0');
  std::fill(pcm.begin(), pcm.end(), 0u);
  return static_cast<int>(exit_code);
}

} // namespace

int h2_gizclaw_e2e_desktop_main(int argc, char **argv) {
  try {
    return run_desktop(argc, argv);
  } catch (...) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=host-exception\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
}

#ifndef H2_GIZCLAW_E2E_DESKTOP_NO_MAIN
int main(int argc, char **argv) {
  return h2_gizclaw_e2e_desktop_main(argc, argv);
}
#endif
