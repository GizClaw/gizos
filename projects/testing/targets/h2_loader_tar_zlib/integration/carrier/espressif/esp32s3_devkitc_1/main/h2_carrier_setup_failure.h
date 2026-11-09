#ifndef H2_CARRIER_SETUP_FAILURE_H
#define H2_CARRIER_SETUP_FAILURE_H

#include "h2_carrier_device_inputs.h"

/* Clear the owned provisioning inputs before reporting and parking while
 * the independent H2Loader management tasks remain available. */
_Noreturn void h2_carrier_setup_fail(h2_carrier_device_inputs_t* inputs,
                                     const char* stage, int result);

#endif
