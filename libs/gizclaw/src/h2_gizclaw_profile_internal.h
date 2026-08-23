#ifndef H2_GIZCLAW_PROFILE_INTERNAL_H
#define H2_GIZCLAW_PROFILE_INTERNAL_H

#include "h2_gizclaw_profile.h"

#include <stddef.h>
#include <stdint.h>

int h2_gizclaw_profile_encode_name_request(h2_gizclaw_str_t name,
                                           uint8_t *out, size_t capacity,
                                           size_t *out_len);
int h2_gizclaw_profile_encode_emoji_request(h2_gizclaw_str_t emoji,
                                            uint8_t *out, size_t capacity,
                                            size_t *out_len);
int h2_gizclaw_profile_decode_info_response(const uint8_t *data, size_t len,
                                            h2_gizclaw_profile_t *out_profile);

#endif
