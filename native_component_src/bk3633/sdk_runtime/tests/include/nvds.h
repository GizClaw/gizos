#ifndef H2_BK3633_NVDS_ABI_TEST_NVDS_H
#define H2_BK3633_NVDS_ABI_TEST_NVDS_H

#include <stdint.h>

#define NVDS_PACKED 1u

typedef uint8_t nvds_tag_len_t;

enum NVDS_STATUS {
    NVDS_OK,
    NVDS_FAIL,
    NVDS_TAG_NOT_DEFINED,
    NVDS_NO_SPACE_AVAILABLE,
    NVDS_LENGTH_OUT_OF_RANGE,
    NVDS_PARAM_LOCKED,
    NVDS_CORRUPT,
};

uint8_t nvds_init(void);
uint8_t nvds_get(uint8_t tag, nvds_tag_len_t *length, uint8_t *data);
uint8_t nvds_put(uint8_t tag, nvds_tag_len_t length, uint8_t *data);
uint8_t nvds_del(uint8_t tag);
uint8_t nvds_lock(uint8_t tag);

#endif
