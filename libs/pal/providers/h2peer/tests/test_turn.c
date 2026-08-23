#include "turn/h2_peer_turn.h"

#include <assert.h>
#include <stdint.h>

static void test_allocation_refresh_uses_server_lifetime(void) {
    h2_peer_turn_refresh_t refresh;
    h2_peer_turn_refresh_init(&refresh);
    assert(!h2_peer_turn_allocation_due(&refresh, UINT64_C(1000000)));

    h2_peer_turn_record_allocation(&refresh, UINT64_C(1000), 100u);
    assert(!h2_peer_turn_allocation_due(&refresh, UINT64_C(80999)));
    assert(h2_peer_turn_allocation_due(&refresh, UINT64_C(81000)));
}

static void test_default_allocation_and_permission_refresh(void) {
    h2_peer_turn_refresh_t refresh;
    h2_peer_turn_refresh_init(&refresh);
    h2_peer_turn_record_allocation(&refresh, 0u, 0u);
    h2_peer_turn_record_permission(&refresh, UINT64_C(5000));

    assert(!h2_peer_turn_allocation_due(&refresh, UINT64_C(479999)));
    assert(h2_peer_turn_allocation_due(&refresh, UINT64_C(480000)));
    assert(!h2_peer_turn_permission_due(&refresh, UINT64_C(244999)));
    assert(h2_peer_turn_permission_due(&refresh, UINT64_C(245000)));
}

static void test_deadline_saturates(void) {
    h2_peer_turn_refresh_t refresh;
    h2_peer_turn_refresh_init(&refresh);
    h2_peer_turn_record_allocation(&refresh, UINT64_MAX - 10u, 600u);
    assert(!h2_peer_turn_allocation_due(&refresh, UINT64_MAX - 1u));
    assert(h2_peer_turn_allocation_due(&refresh, UINT64_MAX));
}

int main(void) {
    test_allocation_refresh_uses_server_lifetime();
    test_default_allocation_and_permission_refresh();
    test_deadline_saturates();
    return 0;
}
