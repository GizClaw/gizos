#include "buffet_cards.h"

static const char* const keys[BF_CARD_COUNT] = {"base-bite", "base-settle",
                                                "story-hospital"};

h2_pal_result_t bf_cards_sync(const h2_pal_pref_api_t* pref, bf_album_t* album,
                              unsigned* pending) {
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(
      pref, BF_CARD_NAMESPACE,
      *pending ? H2_PAL_PREF_OPEN_READ_WRITE : H2_PAL_PREF_OPEN_READ_ONLY, &ns);
  if (rc == H2_PAL_ERR_NOT_FOUND && !*pending) {
    album->unsaved = false;
    return H2_PAL_OK;
  }
  if (rc != H2_PAL_OK || !ns) {
    album->unsaved = true;
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  }
  unsigned stored = 0, staged = 0;
  for (int i = 0; i < BF_CARD_COUNT; i++) {
    uint32_t value = 0;
    unsigned bit = 1u << i;
    int read =
        ns->get_u32 ? ns->get_u32(ns, keys[i], &value) : H2_PAL_ERR_UNSUPPORTED;
    if (read == H2_PAL_OK && value == 1) {
      stored |= bit;
      continue;
    }
    if (read != H2_PAL_ERR_NOT_FOUND) {
      rc = read == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : read;
      break;
    }
  }
  album->owned |= stored;
  if (rc == H2_PAL_OK) {
    staged = *pending & ~stored;
    for (int i = 0; i < BF_CARD_COUNT && rc == H2_PAL_OK; i++)
      if (staged & (1u << i))
        rc = ns->set_u32 ? ns->set_u32(ns, keys[i], 1) : H2_PAL_ERR_UNSUPPORTED;
    if (rc == H2_PAL_OK && staged)
      rc = ns->commit ? ns->commit(ns) : H2_PAL_ERR_UNSUPPORTED;
  }
  int close = ns->close ? ns->close(ns) : H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) rc = close;
  if (rc == H2_PAL_OK) *pending &= ~(stored | staged);
  album->unsaved = rc != H2_PAL_OK;
  return rc;
}
