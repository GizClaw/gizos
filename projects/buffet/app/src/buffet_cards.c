#include "buffet_cards.h"

static const char* const keys[BF_CARD_COUNT] = {"base-bite", "base-settle",
                                                "story-hospital"};

typedef struct {
  const h2_pal_pref_api_t* pref;
  unsigned pending;
  unsigned stored;
  unsigned inserted;
} card_save_t;

/* Only the Pref operation runs under the host lock. The game thread applies
 * the outcome before rendering the receipt or emitting the reveal cues. */
static int sync_store(void* context) {
  card_save_t* save = context;
  h2_pal_pref_namespace_t* ns = NULL;
  int rc = h2_pal_pref_open(
      save->pref, BF_CARD_NAMESPACE,
      save->pending ? H2_PAL_PREF_OPEN_READ_WRITE : H2_PAL_PREF_OPEN_READ_ONLY,
      &ns);
  if (rc == H2_PAL_ERR_NOT_FOUND && !save->pending) return H2_PAL_OK;
  if (rc != H2_PAL_OK || !ns)
    return rc == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : rc;
  for (int i = 0; i < BF_CARD_COUNT; i++) {
    uint32_t value = 0;
    int read =
        ns->get_u32 ? ns->get_u32(ns, keys[i], &value) : H2_PAL_ERR_UNSUPPORTED;
    if (read == H2_PAL_OK && value == 1)
      save->stored |= 1u << i;
    else if (read != H2_PAL_ERR_NOT_FOUND) {
      rc = read == H2_PAL_OK ? H2_PAL_ERR_INVALID_STATE : read;
      break;
    }
  }
  unsigned missing = save->pending & ~save->stored;
  for (int i = 0; i < BF_CARD_COUNT && rc == H2_PAL_OK; i++) {
    if (missing & (1u << i))
      rc = ns->set_u32 ? ns->set_u32(ns, keys[i], 1) : H2_PAL_ERR_UNSUPPORTED;
  }
  if (rc == H2_PAL_OK && missing)
    rc = ns->commit ? ns->commit(ns) : H2_PAL_ERR_UNSUPPORTED;
  int closed = ns->close ? ns->close(ns) : H2_PAL_ERR_INVALID_STATE;
  if (rc == H2_PAL_OK) rc = closed;
  if (rc == H2_PAL_OK) save->inserted = missing;
  return rc;
}

h2_pal_result_t bf_cards_sync(const h2_pal_pref_api_t* pref, bf_album_t* album,
                              unsigned* pending,
                              const h2_buffet_config_t* cfg) {
  card_save_t save = {.pref = pref, .pending = *pending};
  bool guarded = *pending && cfg && cfg->with_card_lock;
  int rc = guarded ? cfg->with_card_lock(cfg->user, sync_store, &save)
                   : sync_store(&save);
  album->owned |= save.stored;
  if (rc != H2_PAL_ERR_WOULD_BLOCK) album->unsaved = rc != H2_PAL_OK;
  if (rc == H2_PAL_OK) {
    *pending = 0;
    bf_confirm_cards(album, save.inserted);
  } else if (!guarded && rc == H2_PAL_ERR_UNSUPPORTED) {
    /* Single-instance hosts without a Pref provider keep a session album. */
    bf_confirm_cards(album, *pending);
  }
  return rc;
}
