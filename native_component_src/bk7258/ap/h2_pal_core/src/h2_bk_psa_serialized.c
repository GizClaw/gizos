#include <os/os.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include <psa/crypto.h>

/* This SDK's PSA key-slot registry has no internal mutex. PAL SRTP and SDK
 * TLS/DTLS share it: concurrent import/destroy can clear a slot between its
 * allocation and memcpy. Serialize the public SDK boundary, including its
 * callers outside PAL, rather than masking interrupts during cryptography.
 * Storage is SRAM; the short initialization critical section is cross-core. */
static StaticSemaphore_t psa_mutex_storage;
static beken_mutex_t psa_mutex;

static int psa_enter(void) {
    const uint32_t state = rtos_enter_critical();
    if (psa_mutex == NULL)
        psa_mutex = (beken_mutex_t)xSemaphoreCreateRecursiveMutexStatic(
            &psa_mutex_storage);
    beken_mutex_t mutex = psa_mutex;
    rtos_exit_critical(state);
    return mutex != NULL && rtos_lock_recursive_mutex(&mutex) == kNoErr;
}

#define H2_BK_PSA_CALL(name, parameters, arguments)                      \
    extern psa_status_t __real_##name parameters;                       \
    psa_status_t __wrap_##name parameters {                             \
        if (!psa_enter())                                               \
            return PSA_ERROR_GENERIC_ERROR;                            \
        const psa_status_t status = __real_##name arguments;            \
        (void)rtos_unlock_recursive_mutex(&psa_mutex);                   \
        return status;                                                 \
    }
#include "h2_bk_psa_calls.h"
#undef H2_BK_PSA_CALL
