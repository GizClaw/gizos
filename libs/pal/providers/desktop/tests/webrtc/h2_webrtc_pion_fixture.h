#ifndef H2_WEBRTC_PION_FIXTURE_H
#define H2_WEBRTC_PION_FIXTURE_H

#include "h2/pal/application/h2_pal_webrtc.h"

#include <stddef.h>
#include <sys/types.h>

typedef struct h2_webrtc_pion_fixture {
    pid_t pid;
    int port;
    int stun_port;
    int turn_port;
    int ice_udp_port;
    int ice_tcp_port;
    char mode[24];
    char turn_username[32];
    char turn_credential[32];
    char session_id[32];
} h2_webrtc_pion_fixture_t;

typedef struct h2_webrtc_turn_stats {
    unsigned long long allocations_created;
    unsigned long long allocations_deleted;
    unsigned long long permissions_created;
    unsigned long long channels_created;
    unsigned long long relay_ingress;
    unsigned long long relay_egress;
} h2_webrtc_turn_stats_t;

typedef struct h2_webrtc_channel_stats {
    unsigned long long created;
    unsigned long long opened;
    unsigned long long closed;
    unsigned long long current;
    unsigned long long max_current;
    unsigned long long reverse_replies;
} h2_webrtc_channel_stats_t;

typedef struct h2_webrtc_ice_pair {
    char mode[24];
    char local_protocol[8];
    char remote_protocol[8];
    char local_type[16];
    char remote_type[16];
    char local_tcp_type[16];
    char remote_tcp_type[16];
    unsigned long long udp_drops;
} h2_webrtc_ice_pair_t;

int h2_webrtc_pion_fixture_start(h2_webrtc_pion_fixture_t *fixture,
                                 const char *server_path, const char *mode);
int h2_webrtc_pion_fixture_exchange(h2_webrtc_pion_fixture_t *fixture,
                                    h2_pal_webrtc_str_t offer, char *answer,
                                    size_t answer_cap, size_t *answer_len,
                                    int relay_only);
int h2_webrtc_pion_fixture_exchange_performance(
    h2_webrtc_pion_fixture_t *fixture, h2_pal_webrtc_str_t offer,
    char *answer, size_t answer_cap, size_t *answer_len);
int h2_webrtc_pion_fixture_close_session(h2_webrtc_pion_fixture_t *fixture);
int h2_webrtc_pion_fixture_turn_stats(const h2_webrtc_pion_fixture_t *fixture,
                                      h2_webrtc_turn_stats_t *stats);
int h2_webrtc_pion_fixture_channel_stats(
    const h2_webrtc_pion_fixture_t *fixture, h2_webrtc_channel_stats_t *stats);
int h2_webrtc_pion_fixture_ice_pair(const h2_webrtc_pion_fixture_t *fixture,
                                    h2_webrtc_ice_pair_t *pair);
void h2_webrtc_pion_fixture_stop(h2_webrtc_pion_fixture_t *fixture);

#endif
