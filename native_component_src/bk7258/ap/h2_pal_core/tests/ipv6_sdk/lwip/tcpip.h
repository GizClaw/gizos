#include "lwip/priv/tcpip_priv.h"
err_t tcpip_callback_wait(void (*fn)(void *), void *);

err_t tcpip_callback(void (*fn)(void*),void*);
