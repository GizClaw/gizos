#ifndef H2_ALIPAY_PROTOCOL_H
#define H2_ALIPAY_PROTOCOL_H

#include "core/h2_pal_errors.h"
#include "os/h2_pal_mem.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed stable identity strings, NUL-terminated and valid for the call.
 * SN/IMEI contain ASCII letters, digits or underscore. MAC is uppercase
 * XX:XX:XX:XX:XX:XX. model is a nonempty printable product name without #.
 * Callers selecting SN substitution supply the same factory SN as sn and imei.
 */
typedef struct h2_alipay_protocol_identity {
  const char* sn;
  const char* imei;
  const char* mac;
  const char* model;
} h2_alipay_protocol_identity_t;

/** Format gateway Ts as UTC milliseconds truncated to whole seconds, matching
 * the original RPC clock adapter and formatter. The qualified delivery uses
 * positive signed-32-bit epoch seconds. Zero/out-of-range time returns
 * INVALID_STATE and clears out. No allocation occurs; out must be non-NULL. */
h2_pal_result_t h2_alipay_protocol_timestamp(uint64_t utc_ms, char out[24]);

/** Derive 32 signing-key bytes from the identity and server-issued device
 * name. No SDK, allocation, storage or network calls occur. out is cleared
 * on failure; the caller owns and must clear successful key bytes. */
h2_pal_result_t h2_alipay_protocol_signing_key(
    const h2_alipay_protocol_identity_t* identity, const char* device_name,
    uint8_t out[32]);

/** Encode the exact RPC signing input, including base64 of nonempty body.
 * binary selects the empty-body representation only: "" for binary, "[]"
 * for JSON. out receives a NUL terminator; length excludes it and reports
 * the required size on NO_SPACE. NULL out with zero capacity queries size.
 * All pointers are borrowed for this synchronous operation; no overlap.
 * Invalid input leaves output untouched and sets length to zero. */
h2_pal_result_t h2_alipay_protocol_canonical(const char* operation,
                                             const char* timestamp, bool binary,
                                             const uint8_t* body,
                                             size_t body_length, char* out,
                                             size_t capacity, size_t* length);

/** HMAC-SM3 of caller-supplied canonical bytes, encoded as 64 uppercase hex
 * characters plus NUL. No inputs are retained or allocated. key and out
 * must be non-NULL; data may be NULL only for zero length. */
h2_pal_result_t h2_alipay_protocol_signature(const uint8_t key[32],
                                             const uint8_t* data, size_t length,
                                             char out[65]);

/** Encode the current delivery's unsigned DeviceActiveRequest protobuf.
 * Output is raw protobuf, not base64. length reports required size on
 * NO_SPACE; NULL out with zero capacity queries size. No allocation occurs. */
h2_pal_result_t h2_alipay_protocol_registration(
    const h2_alipay_protocol_identity_t* identity, uint8_t* out,
    size_t capacity, size_t* length);

/** Decode one DeviceActiveResponse, accepting bounded unknown protobuf
 * fields and rejecting duplicate/malformed names. out receives a NUL-terminated
 * server-issued device name; capacity must include its terminator. */
h2_pal_result_t h2_alipay_protocol_device_name(const uint8_t* data,
                                               size_t length, char* out,
                                               size_t capacity);

/** Build the child-watch network/ECDH prepareBind JSON for product type 3
 * with the qualified PAYCODE | SQUARE feature mask 0x4001.
 * public_key is the validated P-256 X || Y point, 64 big-endian bytes.
 * feature is the protocol feature mask; compile_timestamp_s is UTC build time,
 * matching the original version suffix. The caller owns *out and frees it
 * through mem; length excludes NUL. Every allocation uses mem. On failure
 * *out is NULL and *length is zero. No SDK or network operation occurs. */
h2_pal_result_t h2_alipay_protocol_prepare_bind(
    const h2_pal_mem_api_t* mem, const h2_alipay_protocol_identity_t* identity,
    const char* company, const char* biztid, uint32_t feature,
    uint32_t compile_timestamp_s, const uint8_t public_key[64], char** out,
    size_t* length);

/** Format child-watch 0x4001 binding payload text from a successful token.
 * This function does not encode QR/barcode matrices or draw pixels.
 * All output is caller-owned; no allocation occurs. Required length excludes
 * NUL and is reported on NO_SPACE. token must be printable and contain no #. */
h2_pal_result_t h2_alipay_protocol_binding_code(
    const h2_alipay_protocol_identity_t* identity, const char* company,
    const char* token, uint32_t feature, uint32_t compile_timestamp_s,
    char* out, size_t capacity, size_t* length);

#ifdef __cplusplus
}
#endif
#endif
