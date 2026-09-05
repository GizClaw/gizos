#include "h2_gizclaw_pet.h"
#include "h2_gizclaw_social.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* The synchronous download wrappers hand their writer to an output adapter
 * that calls it unconditionally, so a missing writer must be rejected at the
 * public entry point before any request exists. */
static void test_pet_pixa_download_rejects_null_writer(void) {
  uint8_t bytes[64];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  h2_gizclaw_pet_pixa_info_t info;
  memset(&info, 0x5a, sizeof(info));
  assert(h2_gizclaw_rpc_pet_pixa_download(NULL, (h2_gizclaw_str_t){"pet", 3u},
                                          NULL, NULL, 1000u, &storage,
                                          &info) == H2_PAL_ERR_INVALID_ARG);
  assert(storage.used == 0u);
  h2_gizclaw_pet_pixa_info_t zero;
  memset(&zero, 0, sizeof(zero));
  assert(memcmp(&info, &zero, sizeof(info)) == 0);
}

static void test_group_audio_download_rejects_null_writer(void) {
  uint8_t bytes[64];
  h2_gizclaw_resp_storage_t storage = {bytes, sizeof(bytes), 0u};
  h2_gizclaw_friend_group_message_audio_info_t info;
  memset(&info, 0x5a, sizeof(info));
  assert(h2_gizclaw_rpc_friend_group_message_audio_download(
             NULL, (h2_gizclaw_str_t){"group", 5u},
             (h2_gizclaw_str_t){"history", 7u}, NULL, NULL, 1000u, &storage,
             &info) == H2_PAL_ERR_INVALID_ARG);
  assert(storage.used == 0u);
  h2_gizclaw_friend_group_message_audio_info_t zero;
  memset(&zero, 0, sizeof(zero));
  assert(memcmp(&info, &zero, sizeof(info)) == 0);
}

int main(void) {
  test_pet_pixa_download_rejects_null_writer();
  test_group_audio_download_rejects_null_writer();
  return 0;
}
