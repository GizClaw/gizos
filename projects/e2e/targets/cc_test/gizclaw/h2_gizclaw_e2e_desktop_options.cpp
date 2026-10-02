#include "h2_gizclaw_e2e_desktop_options.h"

#include "h2_gizclaw_e2e.h"

#include <cstring>

namespace {

uint32_t parse_suite(const char *value) {
  struct Suite {
    const char *name;
    uint32_t mask;
  };
  static constexpr Suite suites[] = {
      {"all", H2_GIZCLAW_E2E_SUITE_ALL},
      {"connectivity", H2_GIZCLAW_E2E_SUITE_CONNECTIVITY},
      {"rpc", H2_GIZCLAW_E2E_SUITE_RPC},
      {"device", H2_GIZCLAW_E2E_SUITE_DEVICE},
      {"firmware", H2_GIZCLAW_E2E_SUITE_FIRMWARE},
      {"voice", H2_GIZCLAW_E2E_SUITE_VOICE},
      {"firmware-voice",
       H2_GIZCLAW_E2E_SUITE_FIRMWARE | H2_GIZCLAW_E2E_SUITE_VOICE},
      {"concurrency", H2_GIZCLAW_E2E_SUITE_CONCURRENCY},
      {"service", H2_GIZCLAW_E2E_SUITE_SERVICE},
      {"resource", H2_GIZCLAW_E2E_SUITE_RESOURCE},
      {"fence", H2_GIZCLAW_E2E_SUITE_FENCE},
  };
  for (const Suite &suite : suites) {
    if (std::strcmp(value, suite.name) == 0)
      return suite.mask;
  }
  return 0;
}

bool digit(char c) { return c >= '0' && c <= '9'; }
bool alnum(char c) {
  return digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool valid_fence_id(const char *value) {
  if (value == nullptr)
    return false;
  const size_t length = std::strlen(value);
  if (length == 0u || length > 64u || value[0] < 'a' || value[0] > 'z')
    return false;
  for (size_t i = 1u; i < length; ++i)
    if (!((value[i] >= 'a' && value[i] <= 'z') || digit(value[i]) ||
          value[i] == '_' || value[i] == '-'))
      return false;
  return true;
}

bool valid_workflow_alias(const char *value) {
  if (value == nullptr)
    return false;
  const size_t length = std::strlen(value);
  if (length == 0u || length > 63u)
    return false;
  bool separator = true;
  for (size_t i = 0u; i < length; ++i) {
    const char ch = value[i];
    if ((ch >= 'a' && ch <= 'z') || digit(ch))
      separator = false;
    else if ((ch == '-' || ch == '.') && !separator)
      separator = true;
    else
      return false;
  }
  return !separator;
}

// Desktop runner accepts a DNS hostname or IPv4 address and an explicit port.
// URL schemes, credentials, paths, whitespace and ambiguous extra colons are
// rejected before an endpoint can enter logs or provider setup.
bool valid_endpoint(const char *value) {
  if (value == nullptr ||
      std::strlen(value) >= H2_GIZCLAW_E2E_ENDPOINT_CAPACITY)
    return false;
  const char *colon = std::strchr(value, ':');
  if (colon == nullptr || colon == value || colon[1] == '\0')
    return false;
  size_t label_length = 0;
  bool numeric_host = true;
  for (const char *p = value; p != colon; ++p) {
    if (*p == '.') {
      if (label_length == 0 || p[-1] == '-')
        return false;
      label_length = 0;
    } else {
      if (!alnum(*p) && *p != '-')
        return false;
      if (label_length == 0 && *p == '-')
        return false;
      if (++label_length > 63u)
        return false;
      numeric_host = numeric_host && digit(*p);
    }
  }
  if (label_length == 0 || colon[-1] == '-')
    return false;
  if (numeric_host) {
    unsigned octet = 0, count = 1;
    for (const char *p = value; p != colon; ++p) {
      if (*p == '.') {
        ++count;
        octet = 0;
      } else {
        octet = octet * 10u + static_cast<unsigned>(*p - '0');
        if (octet > 255u)
          return false;
      }
    }
    if (count != 4u)
      return false;
  }
  unsigned port = 0;
  size_t port_length = 0;
  for (const char *p = colon + 1; *p != '\0'; ++p) {
    if (!digit(*p) || ++port_length > 5u)
      return false;
    port = port * 10u + static_cast<unsigned>(*p - '0');
  }
  return port > 0u && port <= 65535u;
}

} // namespace

const char *h2_gizclaw_e2e_desktop_parse_options(int argc, char **argv,
                                                 const char *token,
                                                 const char *suite_override,
                                                 bool pion,
                                                 H2GizclawDesktopOptions *out) {
  if (out == nullptr)
    return "invalid-output";
  *out = {};
  if (argc < 3 || argv == nullptr || argv[1] == nullptr || argv[2] == nullptr ||
      argv[1][0] == '\0')
    return "missing-input";

  H2GizclawDesktopOptions parsed;
  parsed.pcm_path = argv[1];
  parsed.token = token;
  parsed.suite_name = suite_override != nullptr && suite_override[0] != '\0'
                          ? suite_override
                          : argv[2];
  parsed.suites = parse_suite(parsed.suite_name);
  if (parsed.suites == 0u)
    return "invalid-suite";
  constexpr uint32_t pion_suites = H2_GIZCLAW_E2E_SUITE_DEVICE | H2_GIZCLAW_E2E_SUITE_RPC |
                                   H2_GIZCLAW_E2E_SUITE_FIRMWARE |
                                   H2_GIZCLAW_E2E_SUITE_RESOURCE |
                                   H2_GIZCLAW_E2E_SUITE_VOICE;
  if (pion && (parsed.suites & ~pion_suites) != 0u)
    return "unsupported-pion-suite";
  constexpr char endpoint_flag[] = "--endpoint=";
  constexpr char workflow_flag[] = "--fence-workflow=";
  constexpr char first_flag[] = "--fence-first=";
  constexpr char second_flag[] = "--fence-second=";
  for (int i = 3; i < argc; ++i) {
    if (argv[i] == nullptr)
      return "unknown-argument";
    if (std::strncmp(argv[i], endpoint_flag, sizeof(endpoint_flag) - 1u) == 0) {
      if (parsed.endpoint != nullptr)
        return "duplicate-endpoint";
      parsed.endpoint = argv[i] + sizeof(endpoint_flag) - 1u;
    } else if (std::strncmp(argv[i], workflow_flag,
                            sizeof(workflow_flag) - 1u) == 0) {
      if (parsed.fence_workflow_name != nullptr)
        return "duplicate-fence-workflow";
      parsed.fence_workflow_name = argv[i] + sizeof(workflow_flag) - 1u;
    } else if (std::strncmp(argv[i], first_flag, sizeof(first_flag) - 1u) == 0) {
      if (parsed.fence_first_id != nullptr)
        return "duplicate-fence-first";
      parsed.fence_first_id = argv[i] + sizeof(first_flag) - 1u;
    } else if (std::strncmp(argv[i], second_flag,
                            sizeof(second_flag) - 1u) == 0) {
      if (parsed.fence_second_id != nullptr)
        return "duplicate-fence-second";
      parsed.fence_second_id = argv[i] + sizeof(second_flag) - 1u;
    } else {
      return "unknown-argument";
    }
  }
  if (parsed.endpoint == nullptr)
    return "missing-endpoint";
  if (!valid_endpoint(parsed.endpoint))
    return "invalid-endpoint";
  if (token == nullptr || token[0] == '\0')
    return "missing-token";
  if (std::strlen(token) > H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX)
    return "oversized-token";
  if (parsed.suites == H2_GIZCLAW_E2E_SUITE_FENCE) {
    if (!valid_workflow_alias(parsed.fence_workflow_name) ||
        !valid_fence_id(parsed.fence_first_id) ||
        !valid_fence_id(parsed.fence_second_id) ||
        std::strcmp(parsed.fence_first_id, parsed.fence_second_id) == 0)
      return "invalid-fence-fixture";
  } else if (parsed.fence_workflow_name != nullptr ||
             parsed.fence_first_id != nullptr ||
             parsed.fence_second_id != nullptr) {
    return "unexpected-fence-fixture";
  }
  *out = parsed;
  return nullptr;
}
