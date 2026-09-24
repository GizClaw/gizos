#include "h2_gizclaw_e2e_desktop.h"
#include "h2_gizclaw_e2e_desktop_options.h"

#include "h2_corehttp.h"
#include "h2_desktop_app_support.h"
#include "h2_desktop_platform.h"
#include "h2_gizclaw_e2e.h"
#include "h2_atomic.h"
#ifdef H2_GIZCLAW_E2E_USE_PION
#include "h2_pion.h"
#endif

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

h2_atomic_bool_t g_stop_requested = {};
h2_atomic_flag_t g_running = {};
bool g_running_ready = false;
#ifndef _WIN32
std::mutex g_signal_read_mutex;
int g_signal_read_fd = -1;
volatile std::sig_atomic_t g_signal_write_fd = -1;
#else
volatile LONG g_signal_seen = 0;
#endif

void request_stop(int) {
#ifndef _WIN32
  const int saved_errno = errno;
  const int fd = g_signal_write_fd;
  if (fd >= 0) {
    const char byte = 1;
    (void)write(fd, &byte, 1);
  }
  errno = saved_errno;
#else
  (void)InterlockedExchange(&g_signal_seen, 1);
#endif
}

bool should_stop(void *) {
#ifndef _WIN32
  {
    std::lock_guard<std::mutex> lock(g_signal_read_mutex);
    if (g_signal_read_fd >= 0) {
      char byte;
      if (read(g_signal_read_fd, &byte, 1) == 1)
        h2_atomic_bool_store(&g_stop_requested, true, H2_ATOMIC_RELAXED);
    }
  }
#else
  if (InterlockedCompareExchange(&g_signal_seen, 0, 0) != 0)
    h2_atomic_bool_store(&g_stop_requested, true, H2_ATOMIC_RELAXED);
#endif
  return h2_atomic_bool_load(&g_stop_requested, H2_ATOMIC_RELAXED);
}

// Own every view the portable runner or a retained Service may borrow. A
// failed stop is not permission to destruct the provider under its live task.
struct DesktopSession {
  std::string endpoint;
  std::string token;
  std::string suite_name;
  std::vector<uint8_t> pcm;
  h2::desktop::OwnedNetworkServices network;
  h2_corehttp_t *http_provider = nullptr;
  h2_pal_http_api_t http_api = {};
#ifdef H2_GIZCLAW_E2E_USE_PION
  h2_pion_t *pion = nullptr;
#endif
  h2_runtime_t *runtime = nullptr;
  h2_gizclaw_e2e_config_t app_config = {};

  int shutdown() {
    if (runtime != nullptr) {
      h2_runtime_deinit(runtime);
      runtime = nullptr;
    }
#ifdef H2_GIZCLAW_E2E_USE_PION
    h2_pion_destroy(&pion);
    if (pion != nullptr)
      return H2_PAL_ERR_IO;
#endif
    h2_corehttp_destroy(http_provider);
    http_provider = nullptr;
    return network.reset();
  }

  ~DesktopSession() {
    (void)shutdown();
    std::fill(token.begin(), token.end(), '\0');
    std::fill(pcm.begin(), pcm.end(), 0u);
  }
};

// Deliberately process-owned on failure: no static destructor, no later run.
DesktopSession *g_retained_session = nullptr;

struct RunGuard {
  bool retain = false;
  ~RunGuard() {
    if (!retain) {
      h2_atomic_bool_destroy(&g_stop_requested);
      h2_atomic_flag_clear(&g_running, H2_ATOMIC_RELEASE);
      (void)h2_gizclaw_e2e_shutdown();
    }
  }
};

struct SignalGuard {
  using Handler = void (*)(int);
  Handler interrupt = SIG_ERR;
  Handler terminate = SIG_ERR;
  bool ready = false;
#ifndef _WIN32
  int read_fd = -1;
  int write_fd = -1;
#endif
  SignalGuard() {
#ifndef _WIN32
    int fds[2];
    if (pipe(fds) != 0)
      return;
    read_fd = fds[0];
    write_fd = fds[1];
    if (fcntl(read_fd, F_SETFL, O_NONBLOCK) == -1 ||
        fcntl(write_fd, F_SETFL, O_NONBLOCK) == -1)
      return;
    {
      std::lock_guard<std::mutex> lock(g_signal_read_mutex);
      g_signal_read_fd = read_fd;
    }
    g_signal_write_fd = write_fd;
#else
    (void)InterlockedExchange(&g_signal_seen, 0);
#endif
    interrupt = std::signal(SIGINT, request_stop);
    terminate = std::signal(SIGTERM, request_stop);
    ready = interrupt != SIG_ERR && terminate != SIG_ERR;
  }
  ~SignalGuard() {
    if (interrupt != SIG_ERR)
      (void)std::signal(SIGINT, interrupt);
    if (terminate != SIG_ERR)
      (void)std::signal(SIGTERM, terminate);
#ifndef _WIN32
    g_signal_write_fd = -1;
    {
      std::lock_guard<std::mutex> lock(g_signal_read_mutex);
      g_signal_read_fd = -1;
      if (read_fd >= 0)
        (void)close(read_fd);
    }
    if (write_fd >= 0)
      (void)close(write_fd);
#endif
  }
};

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
  H2GizclawDesktopOptions options;
  const char *reason = h2_gizclaw_e2e_desktop_parse_options(
      argc, argv, std::getenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN"),
      std::getenv("H2_GIZCLAW_E2E_SUITE"),
#ifdef H2_GIZCLAW_E2E_USE_PION
      true,
#else
      false,
#endif
      &options);
  if (reason != nullptr) {
    std::fprintf(stderr,
                 "H2_GIZCLAW_E2E stage=preflight status=ERROR reason=%s\n",
                 reason);
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  if (!g_running_ready) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=desktop-not-initialized\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  if (h2_atomic_flag_test_and_set(&g_running, H2_ATOMIC_ACQUIRE)) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=active-or-retained-session\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  if (h2_atomic_bool_init(&g_stop_requested, false) != H2_ATOMIC_OK) {
    h2_atomic_flag_clear(&g_running, H2_ATOMIC_RELEASE);
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  if (h2_gizclaw_e2e_init() != H2_PAL_OK) {
    h2_atomic_bool_destroy(&g_stop_requested);
    h2_atomic_flag_clear(&g_running, H2_ATOMIC_RELEASE);
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  RunGuard guard;
  auto session = std::make_unique<DesktopSession>();
  session->endpoint = options.endpoint;
  session->token = options.token;
  session->suite_name = options.suite_name;
  const uint32_t suites = options.suites;
  auto &pcm = session->pcm;
  if ((suites & (H2_GIZCLAW_E2E_SUITE_RPC | H2_GIZCLAW_E2E_SUITE_VOICE)) !=
          0u &&
      !load_pcm(options.pcm_path, &pcm)) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=invalid-voice-input\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }

  auto &network = session->network;
#ifdef H2_GIZCLAW_E2E_USE_PION
  int rc = h2::desktop::open_network_services(false, false, &network);
#else
  int rc = h2::desktop::open_network_services(false, true, &network);
#endif
  auto &http_provider = session->http_provider;
  auto &http_api = session->http_api;
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
  auto &pion = session->pion;
  const h2_pion_config_t pion_config = {
      .mem = h2_desktop_platform_default_allocator(),
      .sync = h2_desktop_platform_sync_api(),
      .task = h2_desktop_platform_task_api(),
      .time = h2_desktop_platform_time_api(),
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

  auto &runtime = session->runtime;
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
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }

  SignalGuard signals;
  if (!signals.ready) {
    std::fprintf(stderr, "H2_GIZCLAW_E2E stage=preflight status=ERROR "
                         "reason=signal-handler-unavailable\n");
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  }
  session->app_config = {
      .server_endpoint = {session->endpoint.data(), session->endpoint.size()},
      .registration_token = {session->token.data(), session->token.size()},
      .device_api_url = std::getenv("H2_GIZCLAW_E2E_DEVICE_API_URL"),
      .device_audio_url = std::getenv("H2_GIZCLAW_E2E_AUDIO_URL"),
      .device_real_audio = false,
      .voice_audio = nullptr,
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
      h2_gizclaw_e2e_run(runtime, &session->app_config, &result);
  if (result.retained_resources == 0u) {
    const int cleanup_rc = session->shutdown();
    if (cleanup_rc != H2_PAL_OK) {
      result.cleanup_rc = cleanup_rc;
      result.retained_resources = 1u;
      result.complete = false;
    }
  }
  // Transfer ownership before reporting. Returning from this function must not
  // invalidate config/PCM/API views still held by a failed-to-stop Service.
  DesktopSession *report_session = session.get();
  if (result.retained_resources != 0u) {
    guard.retain = true;
    g_retained_session = session.release();
  }
  const int final_exit = result.retained_resources != 0u
                             ? H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR
                             : static_cast<int>(exit_code);
  std::printf(
      "H2_GIZCLAW_E2E stage=summary platform=%s endpoint=%s backend=%s "
      "suite=%s profile=%s "
      "selected=%zu terminal=%zu pass=%zu fail=%zu error=%zu blocked=%zu "
      "cancelled=%zu first_failure_case=%s first_failure_rc=%d "
      "cleanup_rc=%d retained_resources=%zu complete=%s exit_code=%d\n",
#if defined(__APPLE__)
      "macos",
#elif defined(_WIN32)
      "windows",
#else
      "linux",
#endif
      report_session->endpoint.c_str(),
#ifdef H2_GIZCLAW_E2E_USE_PION
      "pion",
#else
      "h2peer",
#endif
      report_session->suite_name.c_str(),
      result.runtime_profile_name[0] == '\0' ? "-"
                                             : result.runtime_profile_name,
      result.selected, result.terminal, result.passed, result.failed,
      result.errors, result.blocked, result.cancelled,
      result.first_failure_case[0] == '\0' ? "-" : result.first_failure_case,
      result.first_failure_rc, result.cleanup_rc, result.retained_resources,
      result.complete ? "true" : "false", final_exit);
  std::fflush(stdout);

  return final_exit;
}

} // namespace

int h2_gizclaw_e2e_desktop_init(void) {
  if (g_running_ready) return H2_PAL_ERR_INVALID_STATE;
  const h2_atomic_result_t rc = h2_atomic_flag_init(&g_running);
  if (rc == H2_ATOMIC_UNSUPPORTED) return H2_PAL_ERR_UNSUPPORTED;
  if (rc != H2_ATOMIC_OK) return H2_PAL_ERR_NO_MEMORY;
  g_running_ready = true;
  return H2_PAL_OK;
}

int h2_gizclaw_e2e_desktop_shutdown(void) {
  if (!g_running_ready) return H2_PAL_ERR_INVALID_STATE;
  if (h2_atomic_flag_test_and_set(&g_running, H2_ATOMIC_ACQUIRE))
    return H2_PAL_ERR_BUSY;
  g_running_ready = false;
  h2_atomic_flag_destroy(&g_running);
  return H2_PAL_OK;
}

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
  // Flush each evidence line before concurrent stderr diagnostics are emitted.
  // Configure stdout before any launcher/provider has performed I/O.
  if (std::setvbuf(stdout, nullptr, _IOLBF, 0) != 0)
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  if (h2_gizclaw_e2e_desktop_init() != H2_PAL_OK)
    return H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR;
  const int result = h2_gizclaw_e2e_desktop_main(argc, argv);
  (void)h2_gizclaw_e2e_desktop_shutdown();
  return result;
}
#endif
