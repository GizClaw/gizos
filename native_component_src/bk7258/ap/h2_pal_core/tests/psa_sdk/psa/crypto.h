#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int32_t psa_status_t;
typedef uint32_t mbedtls_svc_key_id_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_derivation_step_t;
typedef struct { int unused; } psa_key_attributes_t;
typedef struct { int unused; } psa_hash_operation_t;
typedef struct { int unused; } psa_mac_operation_t;
typedef struct { int unused; } psa_cipher_operation_t;
typedef struct { int unused; } psa_key_derivation_operation_t;
#define PSA_ERROR_GENERIC_ERROR (-132)
