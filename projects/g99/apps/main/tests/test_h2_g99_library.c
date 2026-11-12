#include "h2_g99_library.h"

#include <assert.h>
#include <stddef.h>

int main(void) {
  h2_g99_library_entry_t entry = {H2_G99_GAME_INTERNAL, true, true, true};
  assert(h2_g99_library_actions(&entry) ==
         (H2_G99_GAME_MOVE_EXTERNAL | H2_G99_GAME_UNINSTALL));
  entry.storage = H2_G99_GAME_EXTERNAL;
  assert(h2_g99_library_actions(&entry) ==
         (H2_G99_GAME_MOVE_INTERNAL | H2_G99_GAME_UNINSTALL));
  entry.can_move_internal = false;
  assert(h2_g99_library_actions(&entry) == H2_G99_GAME_UNINSTALL);
  entry.can_uninstall = false;
  assert(h2_g99_library_actions(&entry) == 0u);
  entry.storage = H2_G99_GAME_FIRMWARE;
  entry.can_move_internal = entry.can_move_external = entry.can_uninstall =
      true;
  assert(h2_g99_library_actions(&entry) == 0u);
  entry.storage = (h2_g99_game_storage_t)255;
  assert(h2_g99_library_actions(&entry) == 0u);
  assert(h2_g99_library_actions(NULL) == 0u);
  return 0;
}
