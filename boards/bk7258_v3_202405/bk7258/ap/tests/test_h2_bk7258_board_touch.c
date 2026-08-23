#include "h2_bk7258_board_private.h"

#include <assert.h>
#include <string.h>

int main(void) {
    const h2_pal_touch_api_t *touch = h2_bk7258_board_touch_api();
    h2_pal_touch_info_t info;
    h2_pal_touch_event_t event;

    assert(touch != NULL);
    assert(touch->vtable != NULL);
    assert(touch != h2_pal_unsupported_touch_api());
    assert(h2_pal_touch_open(touch) == H2_PAL_ERR_UNAVAILABLE);

    memset(&info, 0x5a, sizeof(info));
    assert(h2_pal_touch_get_info(touch, &info) == H2_PAL_ERR_UNAVAILABLE);
    assert(info.width == 0x5a5a5a5au);
    assert(info.height == 0x5a5a5a5au);

    memset(&event, 0x5a, sizeof(event));
    assert(h2_pal_touch_poll_event(touch, &event) == H2_PAL_ERR_UNAVAILABLE);
    assert(event.x == 0x5a5a5a5a);
    assert(event.y == 0x5a5a5a5a);
    assert(h2_pal_touch_close(touch) == H2_PAL_ERR_UNAVAILABLE);
    return 0;
}
