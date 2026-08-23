#ifndef H2_COREHTTP_NO_STANDARD_STREAM_H
#define H2_COREHTTP_NO_STANDARD_STREAM_H

#include <stdio.h>

/* llhttp__debug() is unreachable in the generated production parser. */
#define fprintf(...) ((void)0)

#endif // H2_COREHTTP_NO_STANDARD_STREAM_H
