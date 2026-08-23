#include "h2_sctp_reference_vectors.h"

/*
 * Provenance: RFC 9260 sections 3.1 and 3.3.2, serialized field by field.
 * CRC32C follows RFC 3309 section 2.  The behavior was compared with pinned
 * usrsctp revision 01cc4e042e2235b29d9d489d89728a6f9ac063ed,
 * netinet/sctp_crc32.c.  No upstream object code or runtime is used.
 */
const uint8_t h2_sctp_reference_init_packet[] = {
    0x13u, 0x88u, 0x13u, 0x89u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x59u, 0x57u, 0xc8u, 0x08u, 0x01u, 0x00u, 0x00u, 0x14u,
    0x11u, 0x22u, 0x33u, 0x44u, 0x00u, 0x00u, 0x10u, 0x00u,
    0x00u, 0x03u, 0x00u, 0x04u, 0x01u, 0x02u, 0x03u, 0x04u,
};

const size_t h2_sctp_reference_init_packet_len =
    sizeof(h2_sctp_reference_init_packet);

/* RFC 8260 section 2.1: complete ordered I-DATA message with PPID 53. */
const uint8_t h2_sctp_reference_i_data_chunk[] = {
    0x40u, 0x03u, 0x00u, 0x18u, 0x10u, 0x20u, 0x30u, 0x40u,
    0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x07u,
    0x00u, 0x00u, 0x00u, 0x35u, 0xdeu, 0xadu, 0xbeu, 0xefu,
};
const size_t h2_sctp_reference_i_data_chunk_len =
    sizeof(h2_sctp_reference_i_data_chunk);

/* RFC 8260 section 2.3: one ordered I-FORWARD-TSN stream entry. */
const uint8_t h2_sctp_reference_i_forward_tsn_chunk[] = {
    0xc2u, 0x00u, 0x00u, 0x10u, 0x10u, 0x20u, 0x30u, 0x40u,
    0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x07u,
};
const size_t h2_sctp_reference_i_forward_tsn_chunk_len =
    sizeof(h2_sctp_reference_i_forward_tsn_chunk);

/* RFC 6525 section 4.1: outgoing reset request plus zero wire padding. */
const uint8_t h2_sctp_reference_reset_chunk[] = {
    0x82u, 0x00u, 0x00u, 0x16u, 0x00u, 0x0du, 0x00u, 0x12u,
    0x01u, 0x02u, 0x03u, 0x04u, 0x01u, 0x02u, 0x03u, 0x03u,
    0x10u, 0x20u, 0x30u, 0x40u, 0x00u, 0x02u, 0x00u, 0x00u,
};
const size_t h2_sctp_reference_reset_chunk_len =
    sizeof(h2_sctp_reference_reset_chunk);

/* RFC 9260 section 3.3.10: SHUTDOWN with cumulative TSN acknowledgement. */
const uint8_t h2_sctp_reference_shutdown_chunk[] = {
    0x07u, 0x00u, 0x00u, 0x08u, 0x10u, 0x20u, 0x30u, 0x40u,
};
const size_t h2_sctp_reference_shutdown_chunk_len =
    sizeof(h2_sctp_reference_shutdown_chunk);
