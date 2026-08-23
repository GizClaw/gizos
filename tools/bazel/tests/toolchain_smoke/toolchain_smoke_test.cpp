#include "tools/bazel/tests/toolchain_smoke/smoke_library.h"

#include <cstdlib>

int main()
{
    return h2_toolchain_smoke_add(20, 22) == 42 ? EXIT_SUCCESS : EXIT_FAILURE;
}
