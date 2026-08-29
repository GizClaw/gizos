#include "h2_desktop_app_support.h"
#include "h2_h2loader_e2e_runner.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_cancelled = 0;

struct Options {
  std::string uart;
  std::string ble_id;
  std::string expected_board;
  std::string expected_target;
  std::filesystem::path firmware;
  std::string firmware_url;
  std::string firmware_url_sha256;
  std::string wifi_ssid;
  std::string wifi_password_env;
  std::filesystem::path report;
  std::uint64_t firmware_url_bytes = 0u;
  std::uint32_t repeat = 1u;
  std::uint32_t timeout_ms = 120000u;
  bool rollback = false;
  bool help = false;
};

void signal_handler(int signal_number) {
  (void)signal_number;
  g_cancelled = 1;
}

int is_cancelled(void *user) {
  (void)user;
  return g_cancelled != 0;
}

void usage(const char *program, FILE *stream) {
  std::fprintf(
      stream,
      "usage: %s (--uart ENDPOINT | --ble-id ENDPOINT | both) [options]\n"
      "\n"
      "options:\n"
      "  --expected-board ID          require exact board identity\n"
      "  --expected-target ID         require exact target identity\n"
      "  --firmware FILE              test direct send and stage abort\n"
      "  --firmware-url URL           test device-side URL download\n"
      "  --url-bytes BYTES            expected URL payload size\n"
      "  --url-sha256 HEX             expected URL payload SHA-256\n"
      "  --wifi-ssid SSID             test scan/connect/disconnect\n"
      "  --wifi-password-env NAME     read Wi-Fi password from environment\n"
      "  --rollback                   include disruptive rollback command\n"
      "  --repeat COUNT               repeat every selected transport (default "
      "1)\n"
      "  --timeout-ms MS              connect and command timeout (default "
      "120000)\n"
      "  --report FILE                write a JSON report\n"
      "  --help                       show this help\n",
      program);
}

bool parse_u64(const char *value, std::uint64_t minimum, std::uint64_t maximum,
               std::uint64_t *out) {
  if (value == nullptr || value[0] == '\0' || value[0] == '-')
    return false;
  errno = 0;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum) {
    return false;
  }
  *out = static_cast<std::uint64_t>(parsed);
  return true;
}

bool sha256_valid(const std::string &value) {
  if (value.size() != H2_H2LOADER_HOST_SHA256_HEX_LEN)
    return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool set_once(std::uint64_t bit, std::uint64_t *seen) {
  if ((*seen & bit) != 0u)
    return false;
  *seen |= bit;
  return true;
}

bool parse_options(int argc, char **argv, Options *out) {
  enum : std::uint64_t {
    kUart = 1ull << 0,
    kBle = 1ull << 1,
    kBoard = 1ull << 2,
    kTarget = 1ull << 3,
    kFirmware = 1ull << 4,
    kUrl = 1ull << 5,
    kUrlBytes = 1ull << 6,
    kUrlSha = 1ull << 7,
    kSsid = 1ull << 8,
    kPasswordEnv = 1ull << 9,
    kRepeat = 1ull << 10,
    kTimeout = 1ull << 11,
    kReport = 1ull << 12,
    kRollback = 1ull << 13,
  };
  std::uint64_t seen = 0u;
  for (int index = 1; index < argc; ++index) {
    const char *name = argv[index];
    if (std::strcmp(name, "--help") == 0 || std::strcmp(name, "-h") == 0) {
      out->help = true;
      continue;
    }
    if (std::strcmp(name, "--rollback") == 0) {
      if (!set_once(kRollback, &seen))
        return false;
      out->rollback = true;
      continue;
    }
    if (index + 1 >= argc)
      return false;
    const char *value = argv[++index];
    std::uint64_t bit = 0u;
    if (std::strcmp(name, "--uart") == 0) {
      bit = kUart;
      out->uart = value;
    } else if (std::strcmp(name, "--ble-id") == 0) {
      bit = kBle;
      out->ble_id = value;
    } else if (std::strcmp(name, "--expected-board") == 0) {
      bit = kBoard;
      out->expected_board = value;
    } else if (std::strcmp(name, "--expected-target") == 0) {
      bit = kTarget;
      out->expected_target = value;
    } else if (std::strcmp(name, "--firmware") == 0) {
      bit = kFirmware;
      out->firmware = value;
    } else if (std::strcmp(name, "--firmware-url") == 0) {
      bit = kUrl;
      out->firmware_url = value;
    } else if (std::strcmp(name, "--url-bytes") == 0) {
      bit = kUrlBytes;
      if (!parse_u64(value, 1u, std::numeric_limits<std::uint64_t>::max(),
                     &out->firmware_url_bytes)) {
        return false;
      }
    } else if (std::strcmp(name, "--url-sha256") == 0) {
      bit = kUrlSha;
      out->firmware_url_sha256 = value;
    } else if (std::strcmp(name, "--wifi-ssid") == 0) {
      bit = kSsid;
      out->wifi_ssid = value;
    } else if (std::strcmp(name, "--wifi-password-env") == 0) {
      bit = kPasswordEnv;
      out->wifi_password_env = value;
    } else if (std::strcmp(name, "--repeat") == 0) {
      bit = kRepeat;
      std::uint64_t parsed = 0u;
      if (!parse_u64(value, 1u, 50u, &parsed))
        return false;
      out->repeat = static_cast<std::uint32_t>(parsed);
    } else if (std::strcmp(name, "--timeout-ms") == 0) {
      bit = kTimeout;
      std::uint64_t parsed = 0u;
      if (!parse_u64(value, 1u, 3600000u, &parsed))
        return false;
      out->timeout_ms = static_cast<std::uint32_t>(parsed);
    } else if (std::strcmp(name, "--report") == 0) {
      bit = kReport;
      out->report = value;
    } else {
      return false;
    }
    if (value[0] == '\0' || !set_once(bit, &seen))
      return false;
  }
  if (out->help)
    return true;
  if (out->uart.empty() && out->ble_id.empty())
    return false;
  const bool has_url = !out->firmware_url.empty();
  if (has_url != (out->firmware_url_bytes != 0u) ||
      has_url != !out->firmware_url_sha256.empty() ||
      (has_url && !sha256_valid(out->firmware_url_sha256))) {
    return false;
  }
  if (out->wifi_ssid.empty() != out->wifi_password_env.empty())
    return false;
  const std::size_t cases = 1u + (!out->wifi_ssid.empty() ? 3u : 0u) +
                            (!out->firmware.empty() ? 2u : 0u) +
                            (has_url ? 2u : 0u) + (out->rollback ? 1u : 0u);
  const std::size_t transports =
      (!out->uart.empty() ? 1u : 0u) + (!out->ble_id.empty() ? 1u : 0u);
  return cases * transports * out->repeat <= H2_H2LOADER_E2E_MAX_CASES;
}

std::filesystem::path resolve_input_path(const std::filesystem::path &path) {
  if (path.empty() || path.is_absolute())
    return path;
  const char *working_directory = std::getenv("BUILD_WORKING_DIRECTORY");
  return working_directory == nullptr
             ? std::filesystem::absolute(path)
             : std::filesystem::path(working_directory) / path;
}

bool load_file(const std::filesystem::path &path,
               std::vector<std::uint8_t> *out) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return false;
  out->assign(std::istreambuf_iterator<char>(input), {});
  return !out->empty() && (input.good() || input.eof());
}

std::string json_escape(const std::string &value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20u) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned int>(character) << std::dec;
      } else {
        output << static_cast<char>(character);
      }
    }
  }
  return output.str();
}

void case_event(void *user, const h2_h2loader_e2e_case_result_t *result,
                int started) {
  (void)user;
  if (started) {
    std::printf("H2_LOADER_E2E_CASE state=start transport=%s iteration=%u "
                "name=%s\n",
                h2_h2loader_e2e_transport_name(result->transport),
                result->iteration,
                h2_h2loader_e2e_case_name(result->test_case));
  } else {
    std::printf(
        "H2_LOADER_E2E_CASE state=complete transport=%s iteration=%u "
        "name=%s result=%s rc=%d elapsed_ms=%llu bytes=%llu total=%llu\n",
        h2_h2loader_e2e_transport_name(result->transport), result->iteration,
        h2_h2loader_e2e_case_name(result->test_case),
        result->result == H2_PAL_OK ? "PASS" : "FAIL", result->result,
        static_cast<unsigned long long>(result->elapsed_ms),
        static_cast<unsigned long long>(result->acknowledged_bytes),
        static_cast<unsigned long long>(result->total_bytes));
  }
  std::fflush(stdout);
}

void progress_event(void *user, const h2_h2loader_e2e_case_result_t *result) {
  (void)user;
  const double percent =
      result->total_bytes == 0u
          ? 0.0
          : 100.0 * static_cast<double>(result->acknowledged_bytes) /
                static_cast<double>(result->total_bytes);
  std::printf("H2_LOADER_E2E_PROGRESS transport=%s iteration=%u name=%s "
              "acked=%llu total=%llu percent=%.1f\n",
              h2_h2loader_e2e_transport_name(result->transport),
              result->iteration, h2_h2loader_e2e_case_name(result->test_case),
              static_cast<unsigned long long>(result->acknowledged_bytes),
              static_cast<unsigned long long>(result->total_bytes), percent);
  std::fflush(stdout);
}

bool write_report(const std::filesystem::path &path, const Options &options,
                  const h2_h2loader_e2e_result_t &result) {
  if (path.empty())
    return true;
  const std::filesystem::path resolved = resolve_input_path(path);
  std::ofstream output(resolved, std::ios::binary | std::ios::trunc);
  if (!output)
    return false;
  output << "{\n"
         << "  \"schema\": \"h2loader-e2e-report/v1\",\n"
         << "  \"result\": \"" << (result.result == H2_PAL_OK ? "PASS" : "FAIL")
         << "\",\n"
         << "  \"rc\": " << result.result << ",\n"
         << "  \"uart_endpoint\": \"" << json_escape(options.uart) << "\",\n"
         << "  \"ble_endpoint\": \"" << json_escape(options.ble_id) << "\",\n"
         << "  \"expected_board\": \"" << json_escape(options.expected_board)
         << "\",\n"
         << "  \"expected_target\": \"" << json_escape(options.expected_target)
         << "\",\n"
         << "  \"repeat\": " << options.repeat << ",\n"
         << "  \"firmware\": {\"bytes\": " << result.firmware_bytes
         << ", \"sha256\": \"" << result.firmware_sha256 << "\"},\n"
         << "  \"firmware_url\": {\"bytes\": " << options.firmware_url_bytes
         << ", \"sha256\": \"" << json_escape(options.firmware_url_sha256)
         << "\"},\n"
         << "  \"summary\": {\"cases\": " << result.case_count
         << ", \"passed\": " << result.passed
         << ", \"failed\": " << result.failed
         << ", \"elapsed_ms\": " << result.elapsed_ms << "},\n"
         << "  \"cases\": [\n";
  for (std::size_t index = 0u; index < result.case_count; ++index) {
    const auto &entry = result.cases[index];
    output << "    {\"transport\": \""
           << h2_h2loader_e2e_transport_name(entry.transport)
           << "\", \"iteration\": " << entry.iteration << ", \"name\": \""
           << h2_h2loader_e2e_case_name(entry.test_case) << "\", \"result\": \""
           << (entry.result == H2_PAL_OK ? "PASS" : "FAIL")
           << "\", \"rc\": " << entry.result
           << ", \"terminal\": " << entry.terminal
           << ", \"elapsed_ms\": " << entry.elapsed_ms
           << ", \"acknowledged_bytes\": " << entry.acknowledged_bytes
           << ", \"total_bytes\": " << entry.total_bytes
           << ", \"output_bytes\": " << entry.output_bytes << ", \"status\": ";
    if (entry.status_valid != 0u) {
      output << "{\"board\": \"" << json_escape(entry.status.board)
             << "\", \"target\": \"" << json_escape(entry.status.target)
             << "\", \"chip\": \"" << json_escape(entry.status.chip)
             << "\", \"capabilities\": " << entry.status.capabilities
             << ", \"command_availability\": "
             << entry.status.command_availability << ", \"states\": \"0x"
             << std::hex << std::setw(16) << std::setfill('0')
             << entry.status.states << std::dec << "\"}";
    } else {
      output << "null";
    }
    output << "}" << (index + 1u == result.case_count ? "\n" : ",\n");
  }
  output << "  ]\n}\n";
  output.flush();
  return output.good();
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    usage(argv[0], stderr);
    return 2;
  }
  if (options.help) {
    usage(argv[0], stdout);
    return 0;
  }

  std::vector<std::uint8_t> firmware;
  if (!options.firmware.empty()) {
    options.firmware = resolve_input_path(options.firmware);
    if (!load_file(options.firmware, &firmware)) {
      std::fprintf(stderr, "h2loader-e2e: cannot read firmware: %s\n",
                   options.firmware.string().c_str());
      return 2;
    }
  }
  const char *wifi_password = nullptr;
  if (!options.wifi_password_env.empty()) {
    wifi_password = std::getenv(options.wifi_password_env.c_str());
    if (wifi_password == nullptr) {
      std::fprintf(
          stderr,
          "h2loader-e2e: Wi-Fi password environment variable is not set\n");
      return 2;
    }
  }

  (void)std::signal(SIGINT, signal_handler);
  (void)std::signal(SIGTERM, signal_handler);
  const bool needs_ble = !options.ble_id.empty();
  const h2_pal_system_event_api_t *system_event =
      h2::desktop::system_event_api();
  const h2_pal_ble_host_api_t *ble = nullptr;
  bool system_event_started = false;
  bool ble_started = false;
  h2_pal_result_t rc = H2_PAL_OK;
  if (needs_ble) {
    rc = static_cast<h2_pal_result_t>(h2_pal_system_event_init(system_event));
    system_event_started = rc == H2_PAL_OK;
    if (rc == H2_PAL_OK) {
      ble = h2::desktop::corebluetooth_api();
      rc = h2_pal_ble_start(ble);
      ble_started = rc == H2_PAL_OK;
    }
  }

  h2_runtime_t *runtime = nullptr;
  h2_runtime_config_t runtime_config = h2::desktop::runtime_config(nullptr);
  if (rc == H2_PAL_OK)
    rc = h2_runtime_init(&runtime_config, &runtime);
  h2_h2loader_e2e_result_t result = {};
  if (rc == H2_PAL_OK) {
    const h2_h2loader_e2e_config_t config = {
        .runtime = runtime,
        .serial = h2::desktop::serial_host_api(),
        .ble = ble,
        .uart_endpoint = options.uart.empty() ? nullptr : options.uart.c_str(),
        .ble_endpoint =
            options.ble_id.empty() ? nullptr : options.ble_id.c_str(),
        .expected_board = options.expected_board.empty()
                              ? nullptr
                              : options.expected_board.c_str(),
        .expected_target = options.expected_target.empty()
                               ? nullptr
                               : options.expected_target.c_str(),
        .firmware = firmware.empty() ? nullptr : firmware.data(),
        .firmware_size = firmware.size(),
        .firmware_url = options.firmware_url.empty()
                            ? nullptr
                            : options.firmware_url.c_str(),
        .firmware_url_bytes = options.firmware_url_bytes,
        .firmware_url_sha256 = options.firmware_url_sha256.empty()
                                   ? nullptr
                                   : options.firmware_url_sha256.c_str(),
        .wifi_ssid =
            options.wifi_ssid.empty() ? nullptr : options.wifi_ssid.c_str(),
        .wifi_password = wifi_password,
        .repeat_count = options.repeat,
        .wait_timeout_ms = options.timeout_ms,
        .command_timeout_ms = options.timeout_ms,
        .include_wifi = static_cast<std::uint8_t>(!options.wifi_ssid.empty()),
        .include_send = static_cast<std::uint8_t>(!firmware.empty()),
        .include_send_url =
            static_cast<std::uint8_t>(!options.firmware_url.empty()),
        .include_rollback = static_cast<std::uint8_t>(options.rollback),
        .is_cancelled = is_cancelled,
        .on_case = case_event,
        .on_progress = progress_event,
    };
    rc = h2_h2loader_e2e_run(&config, &result);
  } else {
    result.result = rc;
    result.complete = 1;
  }

  const bool report_ok = write_report(options.report, options, result);
  if (!report_ok) {
    std::fprintf(stderr, "h2loader-e2e: cannot write report: %s\n",
                 options.report.string().c_str());
    if (rc == H2_PAL_OK)
      rc = H2_PAL_ERR_IO;
  }
  std::printf("H2_LOADER_E2E result=%s rc=%d cases=%zu passed=%zu failed=%zu "
              "elapsed_ms=%llu report=%s\n",
              rc == H2_PAL_OK ? "PASS" : "FAIL", rc, result.case_count,
              result.passed, result.failed,
              static_cast<unsigned long long>(result.elapsed_ms),
              options.report.empty() ? "" : options.report.string().c_str());

  if (runtime != nullptr)
    h2_runtime_deinit(runtime);
  if (ble_started)
    (void)h2_pal_ble_stop(ble);
  if (system_event_started)
    h2_pal_system_event_deinit(system_event);
  if (g_cancelled != 0)
    return 130;
  return rc == H2_PAL_OK ? 0 : 1;
}
