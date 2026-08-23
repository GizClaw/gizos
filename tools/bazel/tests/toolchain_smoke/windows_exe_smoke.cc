#include <windows.h>

#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>

int main()
{
    unsigned char random_byte = 0;
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        &random_byte,
        sizeof(random_byte),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        return EXIT_FAILURE;
    }

    std::puts("windows_exe_smoke: ok");
    return EXIT_SUCCESS;
}
