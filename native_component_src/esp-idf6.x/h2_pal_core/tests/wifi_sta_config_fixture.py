"""Compile the production STA config mapper against minimal SDK type stand-ins."""
from pathlib import Path
import sys


def generate(source):
    start = source.index("static void h2_esp_wifi_copy_sta_config(")
    end = source.index("\n}", start) + 2
    return r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "h2/pal/hal/h2_pal_wifi.h"
enum { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN = 1 };
enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY = 1 };
typedef struct {
    struct {
        uint8_t ssid[32], password[64], bssid[6], channel;
        bool bssid_set;
        int scan_method, sort_method;
    } sta;
} wifi_config_t;
''' + source[start:end] + r'''
int main(void) {
    h2_pal_wifi_sta_config_t input = {0};
    wifi_config_t output;
    memcpy(input.ssid, "multi-ap", 8);
    input.ssid_len = 8;
    memcpy(input.password, "test-passphrase", 15);
    input.password_len = 15;
    const uint8_t bssid[6] = {2, 3, 4, 5, 6, 7};
    memcpy(input.bssid, bssid, sizeof(bssid));
    for (unsigned pinned = 0; pinned < 2; ++pinned) {
        for (unsigned channel = 0; channel <= 11; channel += 11) {
            input.bssid_set = (uint8_t)pinned;
            input.channel = (uint8_t)channel;
            memset(&output, 0xa5, sizeof(output));
            h2_esp_wifi_copy_sta_config(&output, &input);
            /* Unpinned SSIDs must consider APs on later channels. A channel
             * hint must not reintroduce the first-match regression. */
            assert(output.sta.scan_method ==
                   (pinned ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN));
            assert(output.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL);
            assert(output.sta.bssid_set == (pinned != 0));
            assert(output.sta.channel == channel);
            assert(memcmp(output.sta.bssid, bssid, sizeof(bssid)) == 0);
            assert(memcmp(output.sta.ssid, input.ssid, 8) == 0);
            assert(output.sta.ssid[8] == 0);
            assert(memcmp(output.sta.password, input.password, 15) == 0);
            assert(output.sta.password[15] == 0);
        }
    }
    return 0;
}
'''


if __name__ == "__main__":
    Path(sys.argv[2]).write_text(generate(Path(sys.argv[1]).read_text()))
