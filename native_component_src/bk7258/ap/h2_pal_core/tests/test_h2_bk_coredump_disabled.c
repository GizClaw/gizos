#include "h2_bk_coredump.h"

#include <assert.h>
#include <stddef.h>

int main(void) {
  h2_bk_coredump_info_t info = {.pc = 123u};
  assert(!h2_bk_coredump_armed());
  assert(h2_bk_coredump_configure(0, 0) == H2_PAL_ERR_UNSUPPORTED);
  assert(h2_bk_coredump_read_info(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_coredump_read_info(&info) == H2_PAL_ERR_UNSUPPORTED);
  assert(info.pc == 0u);
  return 0;
}
