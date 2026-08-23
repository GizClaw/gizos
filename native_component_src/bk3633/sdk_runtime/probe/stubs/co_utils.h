#ifndef H2_BK3633_PROBE_CO_UTILS_H
#define H2_BK3633_PROBE_CO_UTILS_H

#include <stdint.h>

struct bd_addr {
    uint8_t addr[6];
};

extern struct bd_addr co_default_bdaddr;

#endif
