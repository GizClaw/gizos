#ifndef BUFFET_WEB_CARD_LOCK_H
#define BUFFET_WEB_CARD_LOCK_H

#include "h2_buffet.h"

h2_pal_result_t buffet_web_with_card_lock(void* user,
                                          h2_buffet_card_work_fn work,
                                          void* context);
#endif
