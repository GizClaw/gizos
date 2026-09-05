#include "h2_gizclaw_e2e.h"
#include "h2_gizclaw_e2e_desktop_options.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {
void check(std::vector<std::string> values, const char *reason,
           const char *token = "synthetic-token",
           const char *override = nullptr, bool pion = false,
           uint32_t expected_suites = H2_GIZCLAW_E2E_SUITE_ALL) {
  std::vector<char *> argv;
  for (std::string &value : values)
    argv.push_back(value.data());
  H2GizclawDesktopOptions out;
  out.endpoint = "stale";
  const char *actual = h2_gizclaw_e2e_desktop_parse_options(
      static_cast<int>(argv.size()), argv.data(), token, override, pion, &out);
  if (reason != nullptr) {
    assert(actual != nullptr && std::strcmp(actual, reason) == 0);
    assert(out.endpoint == nullptr && out.token == nullptr && out.suites == 0u);
    assert(out.pcm_path == nullptr && out.suite_name == nullptr);
  } else {
    assert(actual == nullptr);
    assert(std::strcmp(out.endpoint, values[3].c_str() + 11u) == 0);
    assert(out.pcm_path == argv[1] && out.token == token);
    assert(std::strcmp(out.suite_name, override != nullptr && *override != '\0'
                                           ? override
                                           : argv[2]) == 0);
    assert(out.suites == expected_suites);
  }
}
} // namespace

int main() {
  const std::string endpoint = "--endpoint=edge-bj-01.e2e.gizclaw.com:9821";
  const std::vector<std::string> valid = {"e2e", "voice.pcm", "all", endpoint};
  check(valid, nullptr);
  struct Suite {
    const char *name;
    uint32_t mask;
  };
  for (const Suite &suite :
       {Suite{"all", H2_GIZCLAW_E2E_SUITE_ALL},
        Suite{"connectivity", H2_GIZCLAW_E2E_SUITE_CONNECTIVITY},
        Suite{"rpc", H2_GIZCLAW_E2E_SUITE_RPC},
        Suite{"firmware", H2_GIZCLAW_E2E_SUITE_FIRMWARE},
        Suite{"voice", H2_GIZCLAW_E2E_SUITE_VOICE},
        Suite{"firmware-voice",
              H2_GIZCLAW_E2E_SUITE_FIRMWARE | H2_GIZCLAW_E2E_SUITE_VOICE},
        Suite{"concurrency", H2_GIZCLAW_E2E_SUITE_CONCURRENCY},
        Suite{"service", H2_GIZCLAW_E2E_SUITE_SERVICE}}) {
    auto args = valid;
    args[2] = suite.name;
    check(args, nullptr, "token", nullptr, false, suite.mask);
  }
  check(valid, nullptr, "token", "rpc", true, H2_GIZCLAW_E2E_SUITE_RPC);
  check(valid, nullptr, "token", "firmware-voice", true,
        H2_GIZCLAW_E2E_SUITE_FIRMWARE | H2_GIZCLAW_E2E_SUITE_VOICE);
  check(valid, "unsupported-pion-suite", "token", nullptr, true);
  check(valid, "unsupported-pion-suite", "token", "service", true);
  check(valid, "invalid-suite", "token", "unknown");
  check(valid, nullptr, "token", "");
  check(valid, "missing-token", nullptr);
  check(valid, "missing-token", "");
  const std::string max_token(H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX, 'x');
  const std::string large_token = max_token + 'x';
  check(valid, nullptr, max_token.c_str());
  check(valid, "oversized-token", large_token.c_str());
  check({"e2e"}, "missing-input");
  check({"e2e", "voice.pcm", "all"}, "missing-endpoint");
  check({"e2e", "", "all", endpoint}, "missing-input");
  check({"e2e", "voice.pcm", "bad", endpoint}, "invalid-suite");
  check({"e2e", "voice.pcm", "all", endpoint, endpoint}, "duplicate-endpoint");
  check({"e2e", "voice.pcm", "all", "--region=bj"}, "unknown-argument");
  check({"e2e", "voice.pcm", "all", endpoint, "--ignored"}, "unknown-argument");
  check({"e2e", "voice.pcm", "all", "--endpoint", "host:9821"},
        "unknown-argument");
  for (const std::string &value :
       std::vector<std::string>{std::string(""),
                                "host",
                                ":9821",
                                "host:",
                                "host:0",
                                "host:65536",
                                "host:-1",
                                "host:+1",
                                "host:9x",
                                "host:123456",
                                "host:9821:9",
                                "https://host:9821",
                                "user:secret@host:9821",
                                "host:9821/path",
                                "host:9821?token=secret",
                                "host:9821\n",
                                "ho st:9821",
                                "a\tb:9821",
                                "-host:9821",
                                "host-:9821",
                                "host..test:9821",
                                ".host:9821",
                                "host.:9821",
                                "ho_st:9821",
                                "127.0.0:9821",
                                "256.0.0.1:9821",
                                "1.2.3.4.5:9821",
                                "999999999999999999999999:9821",
                                "[::1]:9821",
                                "::1:9821",
                                "主机:9821"}) {
    check({"e2e", "voice.pcm", "all", "--endpoint=" + value},
          "invalid-endpoint");
  }
  for (const char *value :
       {"localhost:1", "127.0.0.1:65535", "a-b.example:09821"}) {
    check({"e2e", "voice.pcm", "all", std::string("--endpoint=") + value},
          nullptr);
  }
  const std::string max_endpoint =
      std::string(63, 'a') + "." + std::string(57, 'b') + ":65535";
  assert(max_endpoint.size() == 127u);
  check({"e2e", "voice.pcm", "all", "--endpoint=" + max_endpoint}, nullptr);
  check({"e2e", "voice.pcm", "all",
         "--endpoint=" + std::string(63, 'a') + "." + std::string(58, 'b') +
             ":65535"},
        "invalid-endpoint");
  check({"e2e", "voice.pcm", "all",
         "--endpoint=" + std::string(64, 'a') + ":9821"},
        "invalid-endpoint");
  H2GizclawDesktopOptions out;
  assert(std::strcmp(h2_gizclaw_e2e_desktop_parse_options(4, nullptr, "token",
                                                          nullptr, false, &out),
                     "missing-input") == 0);
  assert(std::strcmp(h2_gizclaw_e2e_desktop_parse_options(
                         0, nullptr, "token", nullptr, false, nullptr),
                     "invalid-output") == 0);
  char program[] = "e2e", pcm[] = "voice.pcm", suite[] = "all";
  char *null_flag[] = {program, pcm, suite, nullptr};
  assert(std::strcmp(h2_gizclaw_e2e_desktop_parse_options(4, null_flag, "token",
                                                          nullptr, false, &out),
                     "unknown-argument") == 0);
  return 0;
}
