#include "h2_device_app_preflight.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

int main() {
  h2_device_app_preflight_t result{};
  assert(h2_device_app_preflight(nullptr, &result) == H2_PAL_ERR_INVALID_ARG);
  return 0;
}
