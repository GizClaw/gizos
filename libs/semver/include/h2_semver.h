#ifndef H2_SEMVER_H
#define H2_SEMVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check a NUL-terminated string against strict SemVer 2.0.0 syntax.
 * NULL, whitespace, a leading 'v', and incomplete versions are invalid.
 * No allocation, input mutation, or fixed numeric/length limit is imposed.
 * The synchronous call does not retain input or wait on external resources.
 * Concurrent calls are safe when callers keep input storage immutable.
 * @param[in] version Borrowed NUL-terminated string, or NULL; valid for the call.
 * @return true for valid SemVer syntax, false otherwise (including empty input).
 */
bool h2_semver_is_valid(const char *version);

/**
 * @brief Compare two NUL-terminated SemVer strings by precedence.
 * On success, return true and set *out_result to -1, 0, or 1 for lhs <, ==, > rhs.
 * Build metadata is validated but does not affect precedence.
 * Invalid versions (including empty strings and NULL) compare equal to each
 * other and lower than every valid version. Use h2_semver_is_valid() to validate.
 * Return false only when out_result is NULL; otherwise return true.
 * No allocation or input retention occurs. The synchronous call does not wait
 * on external resources. Concurrent calls require immutable input storage and
 * separate output storage, or caller-provided synchronization.
 * @param[in] lhs Borrowed NUL-terminated left version, or NULL.
 * @param[in] rhs Borrowed NUL-terminated right version, or NULL.
 * @param[out] out_result Caller-owned storage for the order; valid for the call.
 * @return true when the order is written; false when out_result is NULL.
 */
bool h2_semver_compare(const char *lhs, const char *rhs, int *out_result);

#ifdef __cplusplus
}
#endif

#endif
