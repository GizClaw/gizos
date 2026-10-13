#include "cancellation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <csignal>

int main() {
  using h2::e2e::loader_serial::Cancellation;
  assert(Cancellation::install());
  assert(Cancellation::requested(nullptr) == 0);
  assert(std::raise(SIGINT) == 0);
  assert(Cancellation::requested(nullptr) == 1);
  assert(std::raise(SIGTERM) == 0);
  assert(Cancellation::requested(nullptr) == 1);
  return 0;
}
