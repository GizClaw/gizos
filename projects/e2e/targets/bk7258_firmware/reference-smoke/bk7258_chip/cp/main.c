#include "h2_bk7258_chip.h"

#include "bk_private/bk_init.h"

void _init(void) {}
void _fini(void) {}

int main(void) {
  (void)h2_bk7258_chip_cp_name();
  return bk_init();
}
