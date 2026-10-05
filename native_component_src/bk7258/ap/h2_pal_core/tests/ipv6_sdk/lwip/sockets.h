#include <netinet/in.h>
#include <sys/socket.h>
#ifndef LWIP_IPV6
#define LWIP_IPV6 1
#endif
#if !LWIP_IPV6
#define sockaddr_in6 H2_IPV6_DISABLED_TYPE
#endif
