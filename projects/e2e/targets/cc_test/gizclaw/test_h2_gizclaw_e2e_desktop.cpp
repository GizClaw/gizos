#include "h2_gizclaw_e2e_desktop.h"

#include "h2_gizclaw_e2e.h"

#include <cassert>
#include <cstdlib>
#include <string>

int main() {
  (void)unsetenv("H2_GIZCLAW_E2E_ENTRY");
  (void)unsetenv("H2_GIZCLAW_E2E_SUITE");
  char program[] = "gizclaw-e2e";
  char pcm[] = "unused.pcm";
  char all[] = "all";
  char invalid_suite[] = "invalid";
  char ap[] = "ap";
  char invalid_entry[] = "invalid";
  char *missing_args[] = {program, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(1, missing_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);

  (void)setenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", "test-token", 1);
  char *bad_suite_args[] = {program, pcm, invalid_suite, ap, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, bad_suite_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  char *bad_entry_args[] = {program, pcm, all, invalid_entry, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, bad_entry_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)setenv("H2_GIZCLAW_E2E_SUITE", "invalid", 1);
  char *suite_override_args[] = {program, pcm, all, ap, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, suite_override_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)unsetenv("H2_GIZCLAW_E2E_SUITE");
  (void)setenv("H2_GIZCLAW_E2E_ENTRY", "invalid", 1);
  assert(h2_gizclaw_e2e_desktop_main(4, suite_override_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)unsetenv("H2_GIZCLAW_E2E_ENTRY");

  const std::string oversized(H2_GIZCLAW_E2E_REGISTRATION_TOKEN_MAX + 1u, 'x');
  (void)setenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN", oversized.c_str(), 1);
  char *oversized_args[] = {program, pcm, all, ap, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, oversized_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  (void)unsetenv("H2_GIZCLAW_E2E_REGISTRATION_TOKEN");
  char connectivity[] = "connectivity";
  char *missing_token_args[] = {program, pcm, connectivity, ap, nullptr};
  assert(h2_gizclaw_e2e_desktop_main(4, missing_token_args) ==
         H2_GIZCLAW_E2E_EXIT_HARNESS_ERROR);
  return 0;
}
