#ifndef H2_GIZCLAW_E2E_DESKTOP_OPTIONS_H
#define H2_GIZCLAW_E2E_DESKTOP_OPTIONS_H

#include <cstdint>

// Borrowed views, valid only while argv and environment inputs remain alive.
struct H2GizclawDesktopOptions {
  const char *pcm_path = nullptr;
  const char *endpoint = nullptr;
  const char *token = nullptr;
  const char *suite_name = nullptr;
  uint32_t suites = 0;
};

// Returns a fixed, non-sensitive reason on error, nullptr on success.
// Does not open files, resolve DNS, create providers, or contact a server.
const char *h2_gizclaw_e2e_desktop_parse_options(int argc, char **argv,
                                                 const char *token,
                                                 const char *suite_override,
                                                 bool pion,
                                                 H2GizclawDesktopOptions *out);

#endif
