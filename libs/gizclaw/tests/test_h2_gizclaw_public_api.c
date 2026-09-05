#include "h2_gizclaw.h"

#include <stddef.h>

/* Volatile storage preserves link references to every approved function.
 * No private header or testing macro may be needed by an application. */
static void (*volatile public_api[])(void) = {
#define H2_GIZCLAW_API(name) (void (*)(void)) name,
#include "public_api.inc"
#undef H2_GIZCLAW_API
};

_Static_assert(sizeof(public_api) / sizeof(public_api[0]) == 190u,
               "The approved public function catalog changed");

int main(void) {
  for (size_t i = 0u; i < sizeof(public_api) / sizeof(public_api[0]); ++i)
    if (public_api[i] == NULL)
      return 1;
  return 0;
}
