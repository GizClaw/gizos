#ifndef H2_GIZCLAW_REGISTRATION_INTERNAL_H
#define H2_GIZCLAW_REGISTRATION_INTERNAL_H

#include "h2_gizclaw_registration.h"

int h2_gizclaw_registration_encode_request(const char *token, uint8_t *out,
                                            size_t capacity,
                                            size_t *out_len);

int h2_gizclaw_registration_decode_response(
    const uint8_t *data, size_t len,
    h2_gizclaw_registration_result_t *out_registration);

#endif
