#include "explicit.h"
#include "portable.h"

#include <stdio.h>

int fixture_portable(void) {
  fprintf(stderr, "%s\n", FIRMWARE_COMPONENT_FIXTURE_NAME);
  return fixture_archive();
}
