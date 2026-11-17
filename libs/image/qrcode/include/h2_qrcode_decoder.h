#ifndef H2_QRCODE_DECODER_H
#define H2_QRCODE_DECODER_H

#include "os/h2_pal_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Decode one QR Code Model 2 symbol from an 8-bit luma plane. width/height
 * are positive pixels; stride is bytes per row and is at least width.
 * image_size includes the last row's width bytes. Input storage is borrowed
 * and remains unchanged. Output must not overlap input or length.
 * All temporary allocations use the borrowed Memory PAL and are released
 * before this synchronous call returns. No OS, camera or radio is selected.
 * Successful output contains raw payload bytes without a NUL terminator.
 * length becomes zero on validation, allocation or decoding failure; NO_SPACE
 * instead reports the required payload length. Output bytes are preserved on
 * every failure. NULL output with zero capacity supports measuring a payload.
 * NOT_FOUND means no detected symbol; FORMAT means ambiguous symbols or
 * invalid encoded data. A mirrored grid is retried once before FORMAT.
 */
h2_pal_result_t h2_qrcode_decode_luma(const h2_pal_mem_api_t* memory,
                                      const uint8_t* image, size_t image_size,
                                      size_t width, size_t height,
                                      size_t stride, uint8_t* output,
                                      size_t capacity, size_t* length);

#ifdef __cplusplus
}
#endif
#endif
