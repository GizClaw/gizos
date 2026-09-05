#include "h2_gizclaw_e2e_desktop.h"

#include "h2_gizclaw_e2e.h"

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {
unsigned runs = 0;
bool retain = false;
bool case_failure = false;
const h2_gizclaw_e2e_config_t *saved_config = nullptr;
h2_runtime_t *saved_runtime = nullptr;
void original_signal_handler(int) {}
void set_env(const char *name, const char *value) {
#ifdef _WIN32
  assert(_putenv_s(name, value == nullptr ? "" : value) == 0);
#else
  assert((value == nullptr ? unsetenv(name) : setenv(name, value, 1)) == 0);
#endif
}
} // namespace

// The launcher uses real Desktop providers; this boundary double never opens
// a Peer, sends a packet, or registers anything with an external service.
extern "C" h2_gizclaw_e2e_exit_t
h2_gizclaw_e2e_run(h2_runtime_t *runtime, const h2_gizclaw_e2e_config_t *config,
                   h2_gizclaw_e2e_result_t *result) {
  ++runs;
  assert(runtime != nullptr && runtime->webrtc != nullptr &&
         runtime->http != nullptr);
  assert(config != nullptr && config->should_stop != nullptr);
  assert(!config->should_stop(config->should_stop_user));
  if (runs == 1u) {
    assert(std::raise(SIGINT) == 0);
    assert(config->should_stop(config->should_stop_user));
  }
  // Re-entrant launch must not alter the active signal/stop state or providers.
  char program[] = "e2e", pcm[] = "unused.pcm", suite[] = "firmware";
  char endpoint[] = "--endpoint=localhost:9821";
  char *again[] = {program, pcm, suite, endpoint};
  assert(h2_gizclaw_e2e_desktop_main(4, again) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(config->should_stop(config->should_stop_user) == (runs == 1u));
  *result = {};
  result->selected = result->terminal = 1u;
  result->passed = case_failure ? 0u : 1u;
  result->failed = case_failure ? 1u : 0u;
  result->complete = true;
  if (retain) {
    saved_config = config;
    saved_runtime = runtime;
    result->retained_resources = 1u;
    result->cleanup_rc = H2_PAL_ERR_TIMEOUT;
  }
  return case_failure ? H2_GIZCLAW_E2E_EXIT_CASE_FAILURE
                      : H2_GIZCLAW_E2E_EXIT_PASS;
}

int main() {
  set_env("H2_GIZCLAW_E2E_SUITE", nullptr);
  char program[] = "gizclaw-e2e";
  char pcm[] = "unused.pcm";
  char all[] = "all";
  char invalid_suite[] = "invalid";
  char endpoint[] = "--endpoint=edge-bj-01.e2e.gizclaw.com:9821";
  char invalid_endpoint[] = "--endpoint=invalid";
  char *missing_args[] = {program, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(1, missing_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);

  set_env("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", "test-token");
  char *bad_suite_args[] = {program, pcm, invalid_suite, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, bad_suite_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  char *bad_endpoint_args[] = {program, pcm, all, invalid_endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, bad_endpoint_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  set_env("H2_GIZCLAW_E2E_SUITE", "invalid");
  char *suite_override_args[] = {program, pcm, all, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, suite_override_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  set_env("H2_GIZCLAW_E2E_SUITE", nullptr);
  const std::string oversized(H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX + 1u, 'x');
  set_env("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", oversized.c_str());
  char *oversized_args[] = {program, pcm, all, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, oversized_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  set_env("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", nullptr);
  char firmware[] = "firmware";
  char *missing_token_args[] = {program, pcm, firmware, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(runs == 0u);

  set_env("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", "synthetic-launcher-token");
  auto old_signal = std::signal(SIGINT, original_signal_handler);
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_PASS);
  assert(runs == 1u);
  assert(std::signal(SIGINT, original_signal_handler) ==
         original_signal_handler);
  case_failure = true;
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_CASE_FAILURE);
  assert(runs == 2u);
  case_failure = false;
  const char *tmpdir = std::getenv("TEST_TMPDIR");
  assert(tmpdir != nullptr);
  std::string pcm_path = std::string(tmpdir) + "/launcher.pcm";
  const char samples[] = {1, 0, 2, 0};
  {
    std::ofstream output(pcm_path, std::ios::binary);
    output.write(samples, sizeof(samples));
    assert(output.good());
  }
  char voice[] = "voice";
  missing_token_args[1] = pcm_path.data();
  missing_token_args[2] = voice;
  retain = true;
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(runs == 3u);
  assert(std::signal(SIGINT, original_signal_handler) ==
         original_signal_handler);
  // Overwrite the input views. A retained launch must own its own copies.
  set_env("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", "changed");
  std::memset(endpoint, 'x', sizeof(endpoint) - 1u);
  assert(saved_config != nullptr && saved_runtime != nullptr);
  assert(std::string(saved_config->registration_token.data,
                     saved_config->registration_token.len) ==
         "synthetic-launcher-token");
  assert(std::string(saved_config->server_endpoint.data,
                     saved_config->server_endpoint.len) ==
         "edge-bj-01.e2e.gizclaw.com:9821");
  assert(saved_runtime->http != nullptr &&
         saved_runtime->http->vtable != nullptr);
  assert(saved_runtime->webrtc != nullptr &&
         saved_runtime->webrtc->vtable != nullptr);
  assert(!saved_config->should_stop(saved_config->should_stop_user));
  assert(saved_config->voice_pcm_len == sizeof(samples));
  assert(std::memcmp(saved_config->voice_pcm_s16le_16khz_mono, samples,
                     sizeof(samples)) == 0);
  char retry_endpoint[] = "--endpoint=localhost:9821";
  missing_token_args[3] = retry_endpoint;
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  assert(runs == 3u);
  (void)std::remove(pcm_path.c_str());
  if (old_signal != SIG_ERR)
    (void)std::signal(SIGINT, old_signal);
  return 0;
}
