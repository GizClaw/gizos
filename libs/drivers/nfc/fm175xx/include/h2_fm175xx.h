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
typedef enum h2_fm175xx_activation_phase {
  H2_FM175XX_ACTIVATION_FIELD,
  H2_FM175XX_ACTIVATION_HALT,
  H2_FM175XX_ACTIVATION_REQUEST,
  H2_FM175XX_ACTIVATION_ANTICOLLISION,
  H2_FM175XX_ACTIVATION_SELECT,
  H2_FM175XX_ACTIVATION_COMPLETE
} h2_fm175xx_activation_phase_t;

typedef struct h2_fm175xx_activation_diagnostics {
  h2_fm175xx_activation_phase_t phase;
  uint8_t command;
  uint8_t cascade_level;
  size_t reply_bits;
  int status_result;
  uint8_t last_irq, last_error, last_fifo, last_control;
} h2_fm175xx_activation_diagnostics_t;

/** Same default selection with optional per-call phase/frame observations.
 * Diagnostics are cleared before use and contain no UID or payload. A failed
 * selection samples status after command cleanup without consuming FIFO.
 * status_result is OK only when the complete snapshot succeeded.
 * Result semantics, response budget and serialization are unchanged. */
int h2_fm175xx_type_a_activate_diagnostic(
    h2_fm175xx_t* reader, h2_fm175xx_type_a_card_t* out_card,
    h2_fm175xx_activation_diagnostics_t* diagnostics);
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
/** Non-payload observations of READ attempts during one NDEF operation.
 * Retained CRC frames include malformed CRC replies; last_result preserves
 * the last READ result independently of the enclosing NDEF parser result.
 * required_capacity records the NDEF length after checking the tag extent.
 * A failed READ samples control/status without consuming FIFO bytes.
 * status_result is OK only when that entire snapshot succeeded. */
typedef struct h2_fm175xx_read_diagnostics {
  size_t read_count;
  size_t retained_crc_frames;
  size_t last_reply_bits;
  uint8_t last_page;
  int last_result;
  size_t required_capacity;
  int status_result;
  uint8_t last_irq, last_error, last_fifo, last_control;
} h2_fm175xx_read_diagnostics_t;

/** Read the first NDEF TLV of a selected, formatted Type 2 tag. Complete
 * message bytes exclude the CC/TLV header and stop at that message's end.
 * Later pages need not be readable. Caller serializes selection/I/O. */
int h2_fm175xx_ntag_read_ndef(h2_fm175xx_t* reader, uint8_t* out_data,
                              size_t capacity, size_t* out_len);
/** Same operation with optional per-call diagnostics, cleared before use.
 * No tag UID or content is stored in diagnostics. Ownership and serialization
 * are identical to read_ndef. */
int h2_fm175xx_ntag_read_ndef_diagnostic(
    h2_fm175xx_t* reader, uint8_t* out_data, size_t capacity, size_t* out_len,
    h2_fm175xx_read_diagnostics_t* diagnostics);
/** Replace a selected Type 2 tag's first NDEF TLV and verify it.
 * Supports freely writable CC version 1 tags with NULL/Lock Control/Memory
 * Control prefixes. Prefix bytes and partial-page neighbours are preserved.
 * Static lock bytes and declared dynamic lock bits must be clear. Referenced
 * lock bytes must be readable within the 1024-byte address range. Reserved
 * pages intersecting the TLV stream or replacement are unsupported.
 * Unknown prefix/trailing TLVs are rejected. No lock/config pages are written.
 * Length is invalidated before payload writes and published last. Failure
 * can leave an empty/incomplete message; do not retry a payment implicitly. */
int h2_fm175xx_ntag_write_ndef(h2_fm175xx_t* reader, const uint8_t* data,
                               size_t length);
typedef enum h2_fm175xx_write_phase {
  H2_FM175XX_WRITE_ARGUMENT,
  H2_FM175XX_WRITE_READ_TAG,
  H2_FM175XX_WRITE_CC,
  H2_FM175XX_WRITE_LOCKS,
  H2_FM175XX_WRITE_LAYOUT,
  H2_FM175XX_WRITE_TRAILING_TLV,
  H2_FM175XX_WRITE_CAPACITY,
  H2_FM175XX_WRITE_INVALIDATE,
  H2_FM175XX_WRITE_PAYLOAD,
  H2_FM175XX_WRITE_PUBLISH,
  H2_FM175XX_WRITE_VERIFY,
  H2_FM175XX_WRITE_COMPLETE
} h2_fm175xx_write_phase_t;

typedef struct h2_fm175xx_write_diagnostics {
  h2_fm175xx_write_phase_t phase;
  size_t tag_capacity;
  size_t write_calls;
  size_t ndef_offset;
  size_t control_tlvs;
  uint8_t metadata_valid;
  uint8_t cc[4];
  uint8_t static_locks[2];
  uint8_t first_tlv;
  uint8_t control_tlv[5];
  uint8_t last_page;
} h2_fm175xx_write_diagnostics_t;

/** Same single write/readback with optional per-call diagnostics.
 * Cleared before validation; metadata_valid requires a complete tag read.
 * Only capability/lock/TLV metadata is retained, never UID or message bytes.
 * write_calls counts page-write calls, including failed register setup.
 * No permission check, wire behavior or retry policy is changed. */
int h2_fm175xx_ntag_write_ndef_diagnostic(
    h2_fm175xx_t* reader, const uint8_t* data, size_t length,
    h2_fm175xx_write_diagnostics_t* diagnostics);

#ifdef __cplusplus
}
#endif

#endif
