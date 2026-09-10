#include "h2_semver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void check_order(const char *lhs, const char *rhs, int expected) {
    int result = 42;
    CHECK(h2_semver_compare(lhs, rhs, &result));
    CHECK(result == expected);
    CHECK(h2_semver_compare(rhs, lhs, &result));
    CHECK(result == -expected);
}

int main(void) {
    const char *valid[] = {
        "0.0.0", "1.2.3", "1.0.0-0", "1.0.0-0.3.7",
        "1.0.0-x.7.z.92", "1.0.0-x-y-z.--", "1.0.0-01a",
        "1.0.0+001", "1.0.0-alpha+001", "1.0.0+build.01.-",
        "999999999999999999999999999999.0.0",
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        CHECK(h2_semver_is_valid(valid[i]));
        check_order(valid[i], valid[i], 0);
    }
    const char *invalid[] = {
        NULL, "", "1", "1.2", "1.2.3.4", "v1.2.3", "V1.2.3",
        " 1.2.3", "1.2.3 ", "1.2.3\n", "1.2.3\t", "-1.2.3",
        "01.2.3", "1.02.3", "1.2.03", "1..3", ".2.3", "1.2.",
        "1.2.3-", "1.2.3+", "1.2.3-01", "1.2.3-a.01",
        "1.2.3-.a", "1.2.3-a.", "1.2.3-a..b", "1.2.3-+meta",
        "1.2.3+a..b", "1.2.3+.a", "1.2.3+a.", "1.2.3+a+b",
        "1.2.3-a_b", "1.2.3+a_b", "1.2.3-\x80", "1.2.3+\xff",
        "1.2.3-01+build", "1.2.3-alpha+bad!", "=1.2.3", "1.2.x",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!h2_semver_is_valid(invalid[i]));
        check_order(invalid[i], "0.0.0-0", -1);
        for (size_t j = 0; j < sizeof(valid) / sizeof(valid[0]); ++j) {
            check_order(invalid[i], valid[j], -1);
        }
        for (size_t j = 0; j < sizeof(invalid) / sizeof(invalid[0]); ++j) {
            check_order(invalid[i], invalid[j], 0);
        }
    }
    CHECK(!h2_semver_compare("1.0.0", "2.0.0", NULL));
    CHECK(!h2_semver_compare("", "invalid", NULL));
    CHECK(!h2_semver_compare(NULL, NULL, NULL));
    const char *ordered[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
        "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",
        "1.0.0", "2.0.0", "2.1.0", "2.1.1",
    };
    for (size_t i = 0; i < sizeof(ordered) / sizeof(ordered[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(ordered) / sizeof(ordered[0]); ++j) {
            check_order(ordered[i], ordered[j], -1);
        }
    }
    check_order("1.9.0", "1.10.0", -1);
    check_order("9.99.99", "10.0.0", -1);
    check_order("1.0.9", "1.0.10", -1);
    check_order("1.0.0-A", "1.0.0-a", -1);
    check_order("1.0.0-a", "1.0.0-aa", -1);
    check_order("1.0.0-9", "1.0.0--", -1);
    check_order("1.0.0-999999999999999999999999", "1.0.0-1a", -1);
    check_order("1.0.0-99999999999999999999", "1.0.0-100000000000000000000", -1);
    check_order("18446744073709551615.0.0", "18446744073709551616.0.0", -1);
    check_order("1.0.0+abc", "1.0.0+xyz.001", 0);
    check_order("1.0.0-alpha+abc", "1.0.0-alpha+xyz", 0);
    check_order("1.0.0-alpha+zzz", "1.0.0-alpha.0+aaa", -1);

    char large[4096];
    memset(large, '9', sizeof(large) - 5);
    memcpy(large + sizeof(large) - 5, ".0.0", 5);
    CHECK(h2_semver_is_valid(large));
    check_order(large, "18446744073709551616.0.0", 1);
    check_order(large, large, 0);
    return 0;
}
