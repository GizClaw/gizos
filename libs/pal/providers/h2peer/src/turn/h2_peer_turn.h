#ifndef H2_PEER_TURN_H
#define H2_PEER_TURN_H

#include <stdint.h>

typedef struct h2_peer_turn_refresh {
    uint64_t allocation_due_ms;
    uint64_t permission_due_ms;
} h2_peer_turn_refresh_t;

void h2_peer_turn_refresh_init(h2_peer_turn_refresh_t *refresh);

void h2_peer_turn_record_allocation(
    h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms,
    uint32_t lifetime_s);

void h2_peer_turn_record_permission(
    h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms);

int h2_peer_turn_allocation_due(
    const h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms);

int h2_peer_turn_permission_due(
    const h2_peer_turn_refresh_t *refresh,
    uint64_t now_ms);

#endif
