#ifndef H2_BK7258_E2E_REJECTION_H
#define H2_BK7258_E2E_REJECTION_H

#include "h2/pal/core/h2_pal_errors.h"

#include <cstddef>
#include <string_view>

// Target-owned test diagnostic; deliberately outside portable orchestration.
inline h2_pal_result_t h2_bk7258_e2e_check_rejection(void*, const char* output,
                                                     std::size_t size) {
  if (output == nullptr) return H2_PAL_ERR_INVALID_ARG;
  constexpr std::string_view prefix = "H2_BK_FLASH_REJECTION";
  constexpr std::string_view expected =
      "H2_BK_FLASH_REJECTION valid=1 aes=1 rc=-3 erase_calls=0 write_calls=0";
  std::string_view remaining(output, size);
  bool found = false;
  while (!remaining.empty()) {
    const auto end = remaining.find('\n');
    if (end == std::string_view::npos) {
      // The caller collects through the final STATS checksum line; later
      // unrelated log output can still be incomplete.
      // A partial audit record must still fail, including a duplicate prefix.
      const auto common =
          remaining.size() < prefix.size() ? remaining.size() : prefix.size();
      if (remaining.substr(0, common) == prefix.substr(0, common))
        return H2_PAL_ERR_FORMAT;
      break;
    }
    auto line = remaining.substr(0, end);
    remaining.remove_prefix(end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.substr(0, prefix.size()) != prefix) continue;
    if (found || line != expected) return H2_PAL_ERR_INVALID_STATE;
    found = true;
  }
  return found ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}

#endif
