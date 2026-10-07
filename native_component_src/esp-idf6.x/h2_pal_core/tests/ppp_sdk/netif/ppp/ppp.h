#ifndef H2_TEST_PPP_H
#define H2_TEST_PPP_H
#include <stdint.h>
typedef int err_t;
#define ERR_OK 0
#define ERR_INPROGRESS (-5)
#define PPP_PHASE_DEAD 0u
#define PPP_PHASE_ESTABLISH 6u
#define PPP_PHASE_TERMINATE 11u
#define PPP_PHASE_DISCONNECT 12u
typedef struct ppp_pcb { uint8_t phase; } ppp_pcb;
err_t ppp_close(ppp_pcb *pcb, uint8_t nocarrier);
#endif
