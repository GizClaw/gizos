#include "h2_peer_turn.h"

#include <stddef.h>

#define H2_PEER_TURN_DEFAULT_ALLOCATION_LIFETIME_S UINT32_C(600)
#define H2_PEER_TURN_PERMISSION_LIFETIME_S UINT32_C(300)
#define H2_PEER_TURN_REFRESH_NUMERATOR UINT32_C(4)
#define H2_PEER_TURN_REFRESH_DENOMINATOR UINT32_C(5)

static uint64_t h2_peer_turn_due_ms(
    uint64_t now_ms,
    uint32_t lifetime_s) {
    uint64_t delay_ms =
        (uint64_t)lifetime_s * UINT64_C(1000) *
        H2_PEER_TURN_REFRESH_NUMERATOR /
        H2_PEER_TURN_REFRESH_DENOMINATOR;
    return UINT64_MAX - now_ms < delay_ms
               ? UINT64_MAX
               : now_ms + delay_ms;
}

void h2_peer_turn_refresh_init(h2_peer_turn_refresh_t *refresh) {
    if (refresh != NULL) {
        refresh->allocation_due_ms = UINT64_MAX;
        refresh->permission_due_ms = UINT64_MAX;
    }
}

void h2_peer_turn_record_allocation(
    h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms,
    uint32_t lifetime_s) {
    if (refresh == NULL) {
        return;
    }
    if (lifetime_s == 0u) {
        lifetime_s = H2_PEER_TURN_DEFAULT_ALLOCATION_LIFETIME_S;
    }
    refresh->allocation_due_ms = h2_peer_turn_due_ms(now_ms, lifetime_s);
}

void h2_peer_turn_record_permission(
    h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms) {
    if (refresh != NULL) {
        refresh->permission_due_ms = h2_peer_turn_due_ms(
            now_ms, H2_PEER_TURN_PERMISSION_LIFETIME_S);
    }
}

int h2_peer_turn_allocation_due(
    const h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms) {
    return refresh != NULL && now_ms >= refresh->allocation_due_ms;
}

int h2_peer_turn_permission_due(
    const h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms) {
    return refresh != NULL && now_ms >= refresh->permission_due_ms;
}
