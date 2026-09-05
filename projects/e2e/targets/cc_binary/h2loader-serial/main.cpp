#include "h2_desktop_app_support.h"
#include "h2_desktop_platform.h"
#include "h2_h2loader_serial_e2e.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Options {
  std::uint32_t suites = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT;
  const char *port = nullptr;
  const char *board = nullptr;
  const char *target = nullptr;
  const char *sha256 = nullptr;
  h2_h2loader_host_command_t command = H2_H2LOADER_HOST_COMMAND_STATUS;
  std::uint32_t timeout_ms = 0;
  std::uint32_t reconnect_attempts = 0;
  std::filesystem::path firmware_index;
  std::filesystem::path resource_root;
  std::vector<std::uint8_t> catalog;
};

bool parse_suite(const char *value, std::uint32_t *out) {
  if (std::strcmp(value, "preflight") == 0) {
    *out = H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT;
  } else if (std::strcmp(value, "status") == 0) {
    *out = H2_H2LOADER_SERIAL_E2E_SUITE_STATUS;
  } else if (std::strcmp(value, "command") == 0) {
    *out = H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND;
  } else if (std::strcmp(value, "install") == 0) {
    *out = H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL;
  } else {
    return false;
  }
  return true;
}

bool parse_command(const char *value, h2_h2loader_host_command_t *out) {
  if (std::strcmp(value, "help") == 0) {
    *out = H2_H2LOADER_HOST_COMMAND_HELP;
  } else if (std::strcmp(value, "status") == 0) {
    *out = H2_H2LOADER_HOST_COMMAND_STATUS;
  } else if (std::strcmp(value, "stats") == 0) {
    *out = H2_H2LOADER_HOST_COMMAND_STATS;
  } else if (std::strcmp(value, "memory") == 0) {
    *out = H2_H2LOADER_HOST_COMMAND_MEMORY;
  } else {
    return false;
  }
  return true;
}

bool parse_u32(const char *value, std::uint32_t minimum,
               std::uint32_t maximum, std::uint32_t *out) {
  if (value == nullptr || value[0] == '\0' || value[0] == '-') return false;
  errno = 0;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < minimum ||
      parsed > maximum) {
    return false;
  }
  *out = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parse_options(int argc, char **argv, Options *out) {
  enum : std::uint32_t {
    kSuite = 1u << 0,
    kPort = 1u << 1,
    kBoard = 1u << 2,
    kTarget = 1u << 3,
    kSha256 = 1u << 4,
    kCommand = 1u << 5,
    kFirmwareIndex = 1u << 6,
    kResourceRoot = 1u << 7,
    kTimeout = 1u << 8,
    kReconnectAttempts = 1u << 9,
  };
  std::uint32_t seen = 0u;
  for (int index = 1; index < argc; ++index) {
    if (index + 1 >= argc) return false;
    const char *name = argv[index++];
    const char *value = argv[index];
    std::uint32_t option = 0u;
    if (std::strcmp(name, "--suite") == 0) {
      option = kSuite;
      if (!parse_suite(value, &out->suites)) return false;
    } else if (std::strcmp(name, "--port-id") == 0) {
      option = kPort;
      out->port = value;
    } else if (std::strcmp(name, "--expected-board") == 0) {
      option = kBoard;
      out->board = value;
    } else if (std::strcmp(name, "--expected-target") == 0) {
      option = kTarget;
      out->target = value;
    } else if (std::strcmp(name, "--asset-sha256") == 0) {
      option = kSha256;
      out->sha256 = value;
    } else if (std::strcmp(name, "--command") == 0) {
      option = kCommand;
      if (!parse_command(value, &out->command)) return false;
    } else if (std::strcmp(name, "--firmware-index") == 0) {
      option = kFirmwareIndex;
      out->firmware_index = value;
    } else if (std::strcmp(name, "--resource-root") == 0) {
      option = kResourceRoot;
      out->resource_root = value;
    } else if (std::strcmp(name, "--timeout-ms") == 0) {
      option = kTimeout;
      if (!parse_u32(value, 1u, 600000u, &out->timeout_ms)) return false;
    } else if (std::strcmp(name, "--reconnect-attempts") == 0) {
      option = kReconnectAttempts;
      if (!parse_u32(value, 1u, 1000u, &out->reconnect_attempts)) return false;
    } else {
      return false;
    }
    if ((seen & option) != 0u) return false;
    seen |= option;
  }
  const bool install =
      (out->suites & H2_H2LOADER_SERIAL_E2E_SUITE_INSTALL) != 0u;
  const bool command =
      (out->suites & H2_H2LOADER_SERIAL_E2E_SUITE_COMMAND) != 0u;
  const bool live = out->suites != H2_H2LOADER_SERIAL_E2E_SUITE_PREFLIGHT;
  if (live && (out->port == nullptr || out->port[0] == '\0')) return false;
  if (!command && (seen & kCommand) != 0u) return false;
  if (!install &&
      (seen & (kSha256 | kFirmwareIndex | kResourceRoot |
               kReconnectAttempts)) != 0u) {
    return false;
  }
  if (install) {
    std::error_code index_error;
    std::error_code root_error;
    if (out->board == nullptr || out->board[0] == '\0' ||
        out->target == nullptr || out->target[0] == '\0' ||
        out->sha256 == nullptr || out->firmware_index.empty() ||
        out->resource_root.empty() || !out->firmware_index.is_absolute() ||
        !out->resource_root.is_absolute() ||
        !std::filesystem::is_regular_file(out->firmware_index, index_error) ||
        index_error ||
        !std::filesystem::is_directory(out->resource_root, root_error) ||
        root_error) {
      return false;
    }
    std::ifstream input(out->firmware_index, std::ios::binary);
    out->catalog.assign(std::istreambuf_iterator<char>(input), {});
    return !out->catalog.empty() && (input.good() || input.eof());
  }
  return true;
}

h2_pal_result_t read_resource(void *user, const char *resource_name,
                              std::uint64_t offset, std::uint8_t *out,
                              std::size_t out_size, std::size_t *out_read) {
  if (user == nullptr || resource_name == nullptr || out_read == nullptr ||
      (out == nullptr && out_size != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  const auto *options = static_cast<const Options *>(user);
  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(options->resource_root,
                                                       error);
  const auto path = std::filesystem::weakly_canonical(
      options->resource_root / resource_name, error);
  if (error || path == root ||
      std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first !=
          root.end()) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return H2_PAL_ERR_NOT_FOUND;
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      out_size > static_cast<std::size_t>(
                     std::numeric_limits<std::streamsize>::max())) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  input.seekg(static_cast<std::streamoff>(offset));
  if (out_size == 0u) return H2_PAL_OK;
  input.read(reinterpret_cast<char *>(out),
             static_cast<std::streamsize>(out_size));
  *out_read = static_cast<std::size_t>(input.gcount());
  return input.bad() ? H2_PAL_ERR_IO : H2_PAL_OK;
}

void print_ledger(const h2_h2loader_serial_e2e_result_t &result) {
  for (std::size_t index = 0u; index < result.case_count; ++index) {
    std::printf("H2_DESKTOP_H2LOADER_CASE id=%d rc=%d\n",
                result.cases[index].case_id, result.cases[index].result);
  }
  std::printf("H2_DESKTOP_H2LOADER_INITIAL board=%s target=%s role=%u "
              "version=%s checksum=%s availability=0x%08x\n",
              result.initial_status.board, result.initial_status.target,
              static_cast<unsigned int>(h2_h2loader_host_status_active_role(
                  &result.initial_status)),
              result.initial_status.active_version,
              result.initial_status.active_checksum,
              result.initial_status.command_availability);
  std::printf("H2_DESKTOP_H2LOADER_FINAL board=%s target=%s role=%u "
              "version=%s checksum=%s availability=0x%08x\n",
              result.final_status.board, result.final_status.target,
              static_cast<unsigned int>(h2_h2loader_host_status_active_role(
                  &result.final_status)),
              result.final_status.active_version,
              result.final_status.active_checksum,
              result.final_status.command_availability);
  std::printf("H2_DESKTOP_H2LOADER_METRICS command_bytes=%zu "
              "command_transport=%d command_terminal=%d "
              "command_truncated=%u command_lifecycle=%u "
              "acknowledged=%llu total=%llu elapsed_ms=%llu cleanup=%d "
              "complete=%d\n",
              result.command_output_bytes,
              static_cast<int>(result.command_transport_result),
              static_cast<int>(result.command_terminal),
              static_cast<unsigned int>(result.command_output_truncated),
              static_cast<unsigned int>(result.command_lifecycle_transition),
              static_cast<unsigned long long>(result.acknowledged_bytes),
              static_cast<unsigned long long>(result.total_bytes),
              static_cast<unsigned long long>(result.elapsed_ms),
              result.cleanup_result, result.complete);
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    std::fprintf(stderr,
                 "usage: %s [--suite preflight|status|command|install] "
                 "[--port-id ID] [--expected-board ID] "
                 "[--expected-target ID] "
                 "[--command help|status|stats|memory] "
                 "[--timeout-ms MS] [--reconnect-attempts COUNT] "
                 "[--asset-sha256 HEX --firmware-index ABS_FILE "
                 "--resource-root ABS_DIR]\n",
                 argv[0]);
    return 2;
  }
  h2_runtime_config_t runtime_config = h2::desktop::runtime_config(nullptr);
  h2_runtime_t *runtime = nullptr;
  h2_pal_result_t result = h2_runtime_init(&runtime_config, &runtime);
  h2_h2loader_serial_e2e_result_t e2e = {};
  if (result == H2_PAL_OK) {
    h2_h2loader_serial_e2e_config_t config = {};
    config.suite_mask = options.suites;
    config.serial = h2::desktop::serial_host_api();
    config.port_id = options.port;
    config.expected_board = options.board;
    config.expected_target = options.target;
    config.command = options.command;
    config.catalog_json =
        options.catalog.empty() ? nullptr : options.catalog.data();
    config.catalog_json_len = options.catalog.size();
    config.asset_sha256 = options.sha256;
    config.read_resource = read_resource;
    config.resource_user = &options;
    config.handshake_timeout_ms = options.timeout_ms;
    config.command_timeout_ms = options.timeout_ms;
    config.reconnect_attempts = options.reconnect_attempts;
    result = h2_h2loader_serial_e2e_run(runtime, &config, &e2e);
  }
  print_ledger(e2e);
  std::printf(
      "H2_DESKTOP_H2LOADER_SERIAL_E2E result=%s rc=%d cases=%zu "
      "passed=%zu failed=%zu skipped=%zu ports=%zu bytes=%llu "
      "elapsed_ms=%llu cleanup=%d\n",
      result == H2_PAL_OK ? "PASS" : "FAIL", result, e2e.selected, e2e.passed,
      e2e.failed, e2e.skipped, e2e.enumerated_ports,
      static_cast<unsigned long long>(e2e.acknowledged_bytes),
      static_cast<unsigned long long>(e2e.elapsed_ms), e2e.cleanup_result);
  if (runtime != nullptr) h2_runtime_deinit(runtime);
  return result == H2_PAL_OK ? 0 : 1;
}
