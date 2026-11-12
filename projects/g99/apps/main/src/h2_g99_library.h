#ifndef H2_G99_LIBRARY_H
#define H2_G99_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum h2_g99_game_storage {
  H2_G99_GAME_FIRMWARE = 0,
  H2_G99_GAME_INTERNAL,
  H2_G99_GAME_EXTERNAL,
} h2_g99_game_storage_t;

enum {
  H2_G99_GAME_MOVE_INTERNAL = 1u << 0,
  H2_G99_GAME_MOVE_EXTERNAL = 1u << 1,
  H2_G99_GAME_UNINSTALL = 1u << 2,
};

typedef struct h2_g99_library_entry {
  h2_g99_game_storage_t storage;
  bool can_move_internal;
  bool can_move_external;
  bool can_uninstall;
} h2_g99_library_entry_t;

/** Returns executable actions for the current location. Capability flags
 * require a working package-management operation, not only free-space stats.
 * Firmware entries are immutable and expose no management actions. */
uint8_t h2_g99_library_actions(const h2_g99_library_entry_t* entry);

#ifdef __cplusplus
}
#endif

#endif
