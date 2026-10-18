#include "h2_gizclaw_e2e.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

int main(void) {
  assert(strcmp(h2_gizclaw_e2e_fixture_key(), "media.songs.en-us") == 0);
  assert(strcmp(h2_gizclaw_e2e_fixture_profile(), "fixture-profile") == 0);
  assert(strcmp(h2_gizclaw_e2e_fixture_value(), "fixture-value") == 0);
  assert(strcmp(h2_gizclaw_e2e_fixture_lua_archive_sha256(),
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
  assert(strcmp(h2_gizclaw_e2e_fixture_lua_entry_sha256(),
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0);
  assert(strcmp(h2_gizclaw_e2e_fixture_lua_frame_sha256(),
                "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc") == 0);
  return 0;
}
