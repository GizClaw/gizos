#ifndef H2_FM175XX_H
#define H2_FM175XX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_FM175XX_OK 0
#define H2_FM175XX_ERR_INVALID_ARG (-1)
#define H2_FM175XX_ERR_UNAVAILABLE (-2)
#define H2_FM175XX_ERR_IO (-3)
#define H2_FM175XX_ERR_NO_MEMORY (-4)
#define H2_FM175XX_ERR_TIMEOUT (-5)
#define H2_FM175XX_ERR_PROTOCOL (-6)

#define H2_FM175XX_UID_MAX_LEN 15u

typedef struct h2_fm175xx_transport {
    void *user;
    int (*write_reg)(void *user, uint8_t reg, uint8_t value);
    int (*write_regs)(void *user, uint8_t reg, const uint8_t *data, size_t len);
    int (*read_reg)(void *user, uint8_t reg, uint8_t *out_value);
    int (*read_regs)(void *user, uint8_t reg, uint8_t *out_data, size_t len);
    void (*sleep_ms)(void *user, uint32_t ms);
} h2_fm175xx_transport_t;

typedef struct h2_fm175xx {
    h2_fm175xx_transport_t transport;
} h2_fm175xx_t;

typedef struct h2_fm175xx_type_a_card {
    uint8_t atqa[2];
    uint8_t uid[H2_FM175XX_UID_MAX_LEN];
    uint8_t uid_len;
    uint8_t sak[3];
    uint8_t sak_len;
} h2_fm175xx_type_a_card_t;

int h2_fm175xx_init(h2_fm175xx_t *reader, const h2_fm175xx_transport_t *transport);
int h2_fm175xx_open_type_a(h2_fm175xx_t *reader);
int h2_fm175xx_type_a_activate(h2_fm175xx_t *reader, h2_fm175xx_type_a_card_t *out_card);
int h2_fm175xx_ntag_read_all(h2_fm175xx_t *reader, uint8_t *out_data, size_t capacity, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
