#ifndef H2_TEST_TCPIP_API
#define H2_TEST_TCPIP_API
#include "lwip/netif.h"
struct tcpip_api_call_data {
  int unused;
};
err_t tcpip_api_call(err_t (*fn)(struct tcpip_api_call_data *),
                     struct tcpip_api_call_data *);
#endif
