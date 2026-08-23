#include "h2_desktop_app_support.h"

#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
  std::vector<std::uint8_t> resource;
  if (!h2::desktop::load_resource(
          "libs/pal/providers/desktop/app_support/tests/resource.txt",
          &resource)) {
    std::fputs("resource load failed\n", stderr);
    return 1;
  }
  static const std::vector<std::uint8_t> expected = {
      'd', 'e', 's', 'k', 't', 'o', 'p', '-', 'r', 'e', 's', 'o', 'u',
      'r', 'c', 'e', '-', 'f', 'i', 'x', 't', 'u', 'r', 'e', '\n'};
  if (resource != expected) {
    std::fputs("resource contents differ\n", stderr);
    return 1;
  }
  return 0;
}
