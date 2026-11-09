#ifndef H2_SCTP_REFERENCE_VECTORS_H
#define H2_SCTP_REFERENCE_VECTORS_H

#include <stddef.h>
#include <stdint.h>

extern const uint8_t h2_sctp_reference_init_packet[];
extern const size_t h2_sctp_reference_init_packet_len;
extern const uint8_t h2_sctp_reference_i_data_chunk[];
extern const size_t h2_sctp_reference_i_data_chunk_len;
extern const uint8_t h2_sctp_reference_i_forward_tsn_chunk[];
extern const size_t h2_sctp_reference_i_forward_tsn_chunk_len;
extern const uint8_t h2_sctp_reference_reset_chunk[];
extern const size_t h2_sctp_reference_reset_chunk_len;
extern const uint8_t h2_sctp_reference_shutdown_chunk[];
extern const size_t h2_sctp_reference_shutdown_chunk_len;

#endif
