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
  void* user;
  int (*write_reg)(void* user, uint8_t reg, uint8_t value);
  int (*write_regs)(void* user, uint8_t reg, const uint8_t* data, size_t len);
  int (*read_reg)(void* user, uint8_t reg, uint8_t* out_value);
  int (*read_regs)(void* user, uint8_t reg, uint8_t* out_data, size_t len);
  void (*sleep_ms)(void* user, uint32_t ms);
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

typedef struct h2_fm175xx_type_a_profile {
  uint8_t rf_config;
  uint8_t rx_threshold;
  /** Keep transmitter drive values established by the chip reset. */
  uint8_t preserve_tx_drive;
} h2_fm175xx_type_a_profile_t;

int h2_fm175xx_init(h2_fm175xx_t* reader,
                    const h2_fm175xx_transport_t* transport);
int h2_fm175xx_open_type_a(h2_fm175xx_t* reader);
/**
 * Select a Type A card for subsequent reads. Restore Type A configuration
 * after a reader reset has disabled RF; configuration failures propagate.
 * Each call ends a preceding selection with HLTA, polls IDLE cards with REQA,
 * and wakes HALT cards with WUPA after a genuine timeout. Repeated polls and
 * a read after a poll can therefore select the same retained card. HLTA silence
 * is expected, including empty error-free RX completions; register/transport
 * errors still propagate. The caller serializes this operation with all other
 * reader operations.
 */
int h2_fm175xx_type_a_activate(h2_fm175xx_t* reader,
                               h2_fm175xx_type_a_card_t* out_card);
/**
 * Same selection with a caller-selected RF response budget per frame, 1..50 ms.
 * This is not a total-operation deadline. The original entry keeps its 1 ms
 * default. Malformed replies and transport errors never become timeouts.
 */
int h2_fm175xx_type_a_activate_timed(h2_fm175xx_t* reader,
                                     h2_fm175xx_type_a_card_t* out_card,
                                     uint32_t timeout_ms);
/**
 * Select with board-selected RFCfgReg and RxThresholdReg values. Reapply the
 * Type A setup without a chip reset before selection, even if RF is already
 * enabled. The per-frame timeout remains 1..50 ms; all frame validation and
 * transport errors are preserved. Existing selection APIs keep their defaults.
 */
int h2_fm175xx_type_a_activate_tuned(h2_fm175xx_t* reader,
                                     h2_fm175xx_type_a_card_t* out_card,
                                     uint32_t timeout_ms, uint8_t rf_config,
                                     uint8_t rx_threshold);
/** Reset/open with a borrowed per-call profile; preserve_tx_drive is 0 or 1.
 * A preserving profile leaves reset transmitter drive untouched on open and
 * does not rewrite it on activation. The reader structure and existing APIs
 * retain their layout and default behavior. Caller serializes all operations.
 */
int h2_fm175xx_open_type_a_profile(h2_fm175xx_t* reader,
                                   const h2_fm175xx_type_a_profile_t* profile);
int h2_fm175xx_type_a_activate_profile(
    h2_fm175xx_t* reader, h2_fm175xx_type_a_card_t* out_card,
    uint32_t timeout_ms, const h2_fm175xx_type_a_profile_t* profile);
int h2_fm175xx_ntag_read_all(h2_fm175xx_t* reader, uint8_t* out_data,
                             size_t capacity, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif
