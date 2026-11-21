#ifndef H2_PASSPORT_RECORDING_H
#define H2_PASSPORT_RECORDING_H

#include "h2_mfg_recording.h"
#include "h2_runtime.h"
#include "h2_loader_boot.h"

/* Borrowed through checked recording close and MFG destruction. */
void h2_passport_recording_bind(h2_runtime_t* runtime, h2_loader_t* loader);

/* Optional full partition/image verification before audio/DSP allocation.
 * Call prepare and the following store.open within ONE serialized Loader
 * operation, holding its operation mutex continuously. Never retain a prepared
 * permit across ticks, mutex release, or retries: prepare again immediately
 * before the next hardware entry. Every prepare, bind and close invalidates the
 * previous permit, and every open attempt consumes it. Open always rechecks the
 * current bindings, Loader metadata, disk geometry and physical partitions; a
 * matching permit skips only the expensive SDK image verifier. Direct open or
 * changed state performs full verification. Prepare never erases/writes flash
 * or creates a recording key; a partially failed open still needs checked
 * close.
 */
int h2_passport_recording_prepare(void);

/* Each open requires a fresh CSPRNG-generated AES-128 key. A nonce is four
 * random bytes, a non-wrapping per-open begin generation (LE32), and frame
 * index (LE32). All recordings and failed/retried begins consume generations,
 * so repeated random prefixes cannot reuse a key/nonce within one open.
 * Counter exhaustion fails before erase; reopen requires a new random key.
 * AAD binds recording and frame indices. Authentication/read failures clear
 * caller plaintext; seal/write failures disable that recording until begin.
 * Entropy failures disable the affected key/recording and wipe partial output.
 * Close wipes key/prefix/scratch before checked erasure and retains ownership
 * if erasure fails. RAM speech buffers/queues belong to the MFG App owner. */
extern const h2_mfg_recording_store_t h2_passport_recording_store;

#endif
