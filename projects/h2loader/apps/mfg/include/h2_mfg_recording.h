#ifndef H2_MFG_RECORDING_H
#define H2_MFG_RECORDING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Optional lossless, temporary PCM store for the shared AEC check.
 *
 * All callbacks are required and return zero on success or a PAL error. They
 * run synchronously on the MFG owner, never on an audio observer callback.
 * open, begin and close run with both audio workers stopped. write_frame runs
 * while recording (the owner drains a bounded callback queue on diagnostics
 * paths), and may also run after both workers stop. read_frame runs during
 * replay. PCM pointers are borrowed only until that callback returns; a store
 * must not retain them. The store and user context are borrowed until close
 * succeeds.
 *
 * open receives the complete measured frame count and native mono S16 samples
 * per frame. Diagnostics use one recording: voice at index 0. Opaque AEC uses
 * two: far-only at 0 and double-talk voice at 1. Every measured sample must be
 * retained without shortening, downsampling or lossy compression.
 *
 * begin resets the selected recording before each recorded phase, including
 * retries, before either worker starts. A backend can pre-erase its region and
 * renew its encryption nonce here. Writes then arrive as complete native frames
 * in increasing frame order. Replay reads one complete frame per playback
 * position and may repeat an entire recording for operator listening.
 *
 * close releases and erases temporary recordings. It must be idempotent and
 * support a partially failed open. A failed close retains the store's resources
 * and all MFG dependencies so cleanup can be retried; it must not invalidate
 * handles needed by that retry.
 */
typedef struct h2_mfg_recording_store {
  void* user;
  int (*open)(void* user, unsigned recording_count, unsigned frames,
              unsigned samples);
  int (*begin)(void* user, unsigned recording);
  int (*write_frame)(void* user, unsigned recording, unsigned frame,
                     const int16_t* pcm, unsigned samples);
  int (*read_frame)(void* user, unsigned recording, unsigned frame,
                    int16_t* pcm, unsigned samples);
  int (*close)(void* user);
} h2_mfg_recording_store_t;

#ifdef __cplusplus
}
#endif
#endif
