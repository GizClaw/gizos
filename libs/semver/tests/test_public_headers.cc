#include "h2_semver.h"

int main() {
    int order = 42;
    return h2_semver_is_valid("1.0.0") &&
                   h2_semver_compare("1.0.0", "1.0.0+build", &order) && order == 0
               ? 0
               : 1;
}
