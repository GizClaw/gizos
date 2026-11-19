#ifndef BUFFET_CARDS_H
#define BUFFET_CARDS_H
#include "buffet_game.h"

#define BF_CARD_NAMESPACE H2_BUFFET_PREF_NAMESPACE
/* One monotone Pref key per catalog ID. A failed or unrecognized stored
 * record is never replaced. Pending grants survive retries in this session. */
h2_pal_result_t bf_cards_sync(const h2_pal_pref_api_t* pref, bf_album_t* album,
                              unsigned* pending);
#endif
