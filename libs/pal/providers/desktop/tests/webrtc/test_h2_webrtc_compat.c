#include "h2_webrtc_compat_scenario.h"

#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s SERVER\n", argv[0]);
        return 2;
    }
    return h2_webrtc_compat_run(argv[1]);
}
