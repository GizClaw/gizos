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
 * or creates a recording key; a partially failed open still needs checked close.
 */
int h2_passport_recording_prepare(void);

extern const h2_mfg_recording_store_t h2_passport_recording_store;

#endif
