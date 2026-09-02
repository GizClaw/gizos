#include "h2_gizclaw_e2e_desktop.h"

#include "h2_gizclaw_e2e.h"

#include <cassert>
#include <cstdlib>
#include <string>

int main() {
  (void)unsetenv("H2_GIZCLAW_E2E_SUITE");
  char program[] = "gizclaw-e2e";
  char pcm[] = "unused.pcm";
  char all[] = "all";
  char invalid_suite[] = "invalid";
  char endpoint[] = "--endpoint=edge-bj-01.e2e.gizclaw.com:9821";
  char invalid_endpoint[] = "--endpoint=invalid";
  char *missing_args[] = {program, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(1, missing_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);

  (void)setenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", "test-token", 1);
  char *bad_suite_args[] = {program, pcm, invalid_suite, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, bad_suite_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  char *bad_endpoint_args[] = {program, pcm, all, invalid_endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, bad_endpoint_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)setenv("H2_GIZCLAW_E2E_SUITE", "invalid", 1);
  char *suite_override_args[] = {program, pcm, all, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, suite_override_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)unsetenv("H2_GIZCLAW_E2E_SUITE");
  const std::string oversized(H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX + 1u, 'x');
  (void)setenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", oversized.c_str(), 1);
  char *oversized_args[] = {program, pcm, all, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, oversized_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)unsetenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN");
  char connectivity[] = "connectivity";
  char *missing_token_args[] = {program, pcm, connectivity, endpoint, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  return 0;
}
