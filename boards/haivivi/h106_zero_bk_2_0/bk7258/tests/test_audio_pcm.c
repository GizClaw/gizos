#include "h2_zero_bk_audio_pcm.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

int main(void) {
  const int16_t primary[] = {INT16_MIN, INT16_MAX, -17, 29};
  const int16_t stereo[] = {INT16_MIN, 1000,      INT16_MAX, -1000,
                            -17,       INT16_MAX, 29,        INT16_MIN};
  int16_t guarded[] = {111, 0, 0, 0, 0, 222};
  assert(h2_zero_bk_audio_extract_primary(primary, 4, 1, guarded + 1, 4));
  assert(memcmp(primary, guarded + 1, sizeof(primary)) == 0);
  assert(guarded[0] == 111 && guarded[5] == 222);
  memset(guarded + 1, 0, sizeof(primary));
  assert(h2_zero_bk_audio_extract_primary(stereo, 8, 2, guarded + 1, 4));
  assert(memcmp(primary, guarded + 1, sizeof(primary)) == 0);
  assert(guarded[0] == 111 && guarded[5] == 222);

  int16_t saved[6];
  memcpy(saved, guarded, sizeof(saved));
  assert(!h2_zero_bk_audio_extract_primary(stereo, 7, 2, guarded + 1, 4));
  assert(!h2_zero_bk_audio_extract_primary(stereo, 8, 0, guarded + 1, 4));
  assert(!h2_zero_bk_audio_extract_primary(stereo, 8, 3, guarded + 1, 4));
  assert(!h2_zero_bk_audio_extract_primary(NULL, 8, 2, guarded + 1, 4));
  assert(!h2_zero_bk_audio_extract_primary(stereo, 8, 2, NULL, 4));
  assert(memcmp(saved, guarded, sizeof(saved)) == 0);
  return 0;
}
