#include "h2_g99_library.h"

#include <stddef.h>

uint8_t h2_g99_library_actions(const h2_g99_library_entry_t* entry) {
  if (entry == NULL || (entry->storage != H2_G99_GAME_INTERNAL &&
                        entry->storage != H2_G99_GAME_EXTERNAL))
    return 0u;
  uint8_t actions = entry->can_uninstall ? H2_G99_GAME_UNINSTALL : 0u;
  if (entry->storage == H2_G99_GAME_INTERNAL && entry->can_move_external)
    actions |= H2_G99_GAME_MOVE_EXTERNAL;
  if (entry->storage == H2_G99_GAME_EXTERNAL && entry->can_move_internal)
    actions |= H2_G99_GAME_MOVE_INTERNAL;
  return actions;
}
