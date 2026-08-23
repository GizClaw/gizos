#ifndef H2_BK3633_PROBE_RWIP_H
#define H2_BK3633_PROBE_RWIP_H

#include <stdint.h>

void rwip_init(uint8_t error);
void rwip_schedule(void);

#endif
