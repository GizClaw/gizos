#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <os/os.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include <psa/crypto.h>
#define H2_BK_PSA_CALL(name, parameters, arguments) \
    psa_status_t __wrap_##name parameters;
#include "h2_bk_psa_calls.h"
#undef H2_BK_PSA_CALL
}

static std::mutex initialization;
static std::recursive_mutex registry;
static std::atomic<int> active{0}, overlaps{0}, calls{0}, created{0};
static std::atomic<bool> fail_create{true}, fail_lock{false}, nested{false};
static thread_local unsigned depth;
static constexpr psa_status_t result = -77;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)

// These two C SDK hooks transfer ownership across separate API calls.
#if defined(__clang__)
#define SDK_LOCK_BOUNDARY __attribute__((no_thread_safety_analysis))
#else
#define SDK_LOCK_BOUNDARY
#endif
extern "C" SDK_LOCK_BOUNDARY uint32_t rtos_enter_critical(void) { initialization.lock(); return 1; }
extern "C" SDK_LOCK_BOUNDARY void rtos_exit_critical(uint32_t state) { CHECK(state == 1); initialization.unlock(); }
extern "C" SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *) {
    if (fail_create.load()) return nullptr;
    ++created;
    return &registry;
}
extern "C" int rtos_lock_recursive_mutex(beken_mutex_t *mutex) {
    CHECK(*mutex == &registry);
    if (fail_lock.load()) return -1;
    registry.lock(); return 0;
}
extern "C" int rtos_unlock_recursive_mutex(beken_mutex_t *mutex) {
    CHECK(*mutex == &registry); registry.unlock(); return 0;
}

/* Model the SDK's process-wide key-slot table: simultaneous public entry
 * is unsafe even when calls originate from different PAL/TLS owners. */
static psa_status_t sdk_call() {
    const bool outer = depth++ == 0;
    if (outer && active.fetch_add(1) != 0) ++overlaps;
    ++calls;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (outer && nested.load()) CHECK(__wrap_psa_crypto_init() == result);
    if (outer) --active;
    --depth;
    return result;
}
extern "C" {
#define H2_BK_PSA_CALL(name, parameters, arguments) \
    psa_status_t __real_##name parameters { return sdk_call(); }
#include "h2_bk_psa_calls.h"
#undef H2_BK_PSA_CALL
}

int main() {
    CHECK(__wrap_psa_crypto_init() == PSA_ERROR_GENERIC_ERROR);
    CHECK(calls == 0);
    fail_create = false;
    using Invoke = psa_status_t (*)();
    const Invoke entry_points[] = {
        [] { return __wrap_psa_crypto_init(); },
        [] { return __wrap_psa_destroy_key(0); },
        [] { return __wrap_psa_import_key(nullptr, nullptr, 0, nullptr); },
        [] { return __wrap_psa_export_key(0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_export_public_key(0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_hash_compute(0, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_hash_setup(nullptr, 0); },
        [] { return __wrap_psa_hash_update(nullptr, nullptr, 0); },
        [] { return __wrap_psa_hash_finish(nullptr, nullptr, 0, nullptr); },
        [] { return __wrap_psa_hash_abort(nullptr); },
        [] { return __wrap_psa_mac_compute(0, 0, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_mac_update(nullptr, nullptr, 0); },
        [] { return __wrap_psa_mac_sign_finish(nullptr, nullptr, 0, nullptr); },
        [] { return __wrap_psa_mac_abort(nullptr); },
        [] { return __wrap_psa_cipher_encrypt_setup(nullptr, 0, 0); },
        [] { return __wrap_psa_cipher_set_iv(nullptr, nullptr, 0); },
        [] { return __wrap_psa_cipher_update(nullptr, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_cipher_finish(nullptr, nullptr, 0, nullptr); },
        [] { return __wrap_psa_cipher_abort(nullptr); },
        [] { return __wrap_psa_aead_encrypt(0, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_aead_decrypt(0, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_sign_message(0, 0, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_verify_message(0, 0, nullptr, 0, nullptr, 0); },
        [] { return __wrap_psa_key_derivation_setup(nullptr, 0); },
        [] { return __wrap_psa_key_derivation_input_bytes(nullptr, 0, nullptr, 0); },
        [] { return __wrap_psa_key_derivation_output_bytes(nullptr, nullptr, 0); },
        [] { return __wrap_psa_key_derivation_abort(nullptr); },
        [] { return __wrap_psa_raw_key_agreement(0, 0, nullptr, 0, nullptr, 0, nullptr); },
        [] { return __wrap_psa_generate_random(nullptr, 0); },
        [] { return __wrap_psa_generate_key(nullptr, nullptr); },
    };
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 6; ++thread) workers.emplace_back([&] {
        for (unsigned round = 0; round < 4; ++round)
            for (auto invoke : entry_points) CHECK(invoke() == result);
    });
    for (auto &worker : workers) worker.join();
    CHECK(created == 1 && overlaps == 0 && active == 0);
    CHECK(calls == 6 * 4 * 30);
    const int before = calls.load();
    fail_lock = true;
    CHECK(__wrap_psa_crypto_init() == PSA_ERROR_GENERIC_ERROR && calls == before);
    fail_lock = false;
    nested = true;
    CHECK(__wrap_psa_crypto_init() == result);
    CHECK(calls == before + 2 && active == 0 && overlaps == 0);
    nested = false;
    CHECK(__wrap_psa_crypto_init() == result); // failure/nesting released the guard
    std::puts("PSA shared registry: 30 boundaries, concurrent owners, failures and recursion PASS");
}
