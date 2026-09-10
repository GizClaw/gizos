#include <assert.h>
#include "layout.h"
int main(void) {
    assert(H2_FIXED_APP_OFFSET + H2_FIXED_APP_SIZE == 0x75f000u);
    assert(H2_FIXED_APP_OFFSET % (68u * 1024u) == 0);
    assert(H2_FIXED_CP_SIZE % (68u * 1024u) == 0);
    assert(H2_FIXED_LOADER_AP_SIZE % (68u * 1024u) == 0);
    assert(H2_FIXED_CONTROL_OFFSET >= 0x760000u && H2_FIXED_CONTROL_OFFSET + 4096u <= 0x780000u);
    h2_fixed_boot_request_t r = {H2_FIXED_REQUEST_MAGIC, H2_FIXED_APP_OFFSET, H2_FIXED_APP_SIZE, 0, H2_FIXED_ERASED};
    r.check = ~(r.magic ^ r.app_offset ^ r.app_size);
    assert(h2_fixed_request_valid(&r));
    r.magic = 0;
    assert(!h2_fixed_request_valid(&r));
    assert(h2_fixed_request_consumed(&r) && h2_fixed_request_failed(&r));
    assert(!h2_fixed_request_boots_app(&r));
    r.confirmed = 0u;
    assert(h2_fixed_request_confirmed(&r) && h2_fixed_request_boots_app(&r));
    r.confirmed = H2_FIXED_ERASED;
    r.magic = H2_FIXED_REQUEST_MAGIC;
    r.app_offset += 4096u;
    r.check = ~(r.magic ^ r.app_offset ^ r.app_size);
    assert(!h2_fixed_request_valid(&r));
    r.app_offset = H2_FIXED_APP_OFFSET;
    r.check = 0xffffffffu;
    assert(!h2_fixed_request_valid(&r));
    return 0;
}
