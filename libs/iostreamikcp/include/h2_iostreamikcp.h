#ifndef H2_IOSTREAMIKCP_H
#define H2_IOSTREAMIKCP_H

#include "h2_iostreamikcp_types.h"
#include "h2/pal/hal/h2_pal_uart_io_stream.h"
#include "h2/pal/hal/h2_pal_usb_jtag_io_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

size_t h2_iostreamikcp_frame_encoded_len(size_t payload_len);
/**
 * Encodes a data or session-control frame.
 *
 * Control flags are mutually exclusive. Their conversation ID must be nonzero,
 * and their payload must be exactly four little-endian bytes mirroring
 * frame->conv. Unknown or combined flags and an invalid control payload return
 * H2_PAL_ERR_INVALID_ARG.
 */
h2_pal_result_t h2_iostreamikcp_frame_encode(
    const h2_iostreamikcp_frame_t *frame,
    uint8_t *out,
    size_t out_size,
    size_t *out_len);
void h2_iostreamikcp_filter_init(h2_iostreamikcp_filter_t *filter);
/**
 * Extracts complete validated frames from a possibly dirty byte stream.
 *
 * The frame and payload passed to on_frame borrow filter-owned storage and are
 * valid only until the callback returns. Invalid control frames are discarded
 * while the filter searches for the next valid magic prefix.
 */
h2_pal_result_t h2_iostreamikcp_filter_input(
    h2_iostreamikcp_filter_t *filter,
    const uint8_t *data,
    size_t len,
    h2_iostreamikcp_frame_fn on_frame,
    void *user);
/** Filter input while delivering bytes proven not to belong to a frame. */
h2_pal_result_t h2_iostreamikcp_filter_input_with_log(
    h2_iostreamikcp_filter_t *filter,
    const uint8_t *data,
    size_t len,
    h2_iostreamikcp_frame_fn on_frame,
    void *frame_user,
    h2_iostreamikcp_log_fn on_log,
    void *log_user);

h2_pal_result_t h2_iostreamikcp_open(
    const h2_iostreamikcp_config_t *config,
    h2_iostreamikcp_t **out_stream);
void h2_iostreamikcp_close(h2_iostreamikcp_t *stream);
h2_pal_result_t h2_iostreamikcp_input(
    h2_iostreamikcp_t *stream,
    const uint8_t *data,
    size_t len);
/**
 * Feeds one frame already decoded by a caller-owned physical stream filter.
 *
 * Only a non-empty data frame whose conversation ID matches the stream is
 * accepted. Invalid metadata returns H2_PAL_ERR_INVALID_ARG; an invalid KCP
 * datagram returns H2_PAL_ERR_FORMAT.
 * Session control frames remain owned by the caller's session state machine.
 */
h2_pal_result_t h2_iostreamikcp_input_frame(
    h2_iostreamikcp_t *stream,
    const h2_iostreamikcp_frame_t *frame);
h2_pal_result_t h2_iostreamikcp_poll(
    h2_iostreamikcp_t *stream,
    uint32_t timeout_ms);
h2_pal_result_t h2_iostreamikcp_update(h2_iostreamikcp_t *stream, uint32_t now_ms);
h2_pal_result_t h2_iostreamikcp_read(
    h2_iostreamikcp_t *stream,
    uint8_t *out,
    size_t out_size,
    size_t *out_len);
h2_pal_result_t h2_iostreamikcp_write(
    h2_iostreamikcp_t *stream,
    const uint8_t *data,
    size_t len);
h2_pal_result_t h2_iostreamikcp_flush(h2_iostreamikcp_t *stream);
h2_pal_result_t h2_iostreamikcp_get_stats(
    h2_iostreamikcp_t *stream,
    h2_iostreamikcp_stats_t *out_stats);

h2_iostreamikcp_io_t h2_iostreamikcp_io_from_uart(const h2_pal_uart_io_stream_api_t *api);
h2_iostreamikcp_io_t h2_iostreamikcp_io_from_usb_jtag(const h2_pal_usb_jtag_io_stream_api_t *api);

#ifdef __cplusplus
}
#endif

#endif
