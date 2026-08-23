#include <stdbool.h>

#include "sdkconfig.h"

#if !CONFIG_WIFI_SCAN_COUNTRY_CODE
/*
 * AVDK v3.1.1's wpa_supplicant references this flag unconditionally even
 * though the country-code scan implementation owns it conditionally.
 */
bool site_survey_cc;
#endif
