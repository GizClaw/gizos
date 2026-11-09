#include "h2_onenet.h"

#include <string.h>

int main(void) {
  char client_id[H2_ONENET_CLIENT_ID_CAPACITY];
  const h2_onenet_identity_t identity = {.network = 0u,
                                         .sn = "demo-device-0001"};
  if (h2_onenet_build_client_id("example-device", &identity, client_id,
                                sizeof(client_id)) != H2_PAL_OK)
    return 1;
  if (strcmp(client_id, "example-device?network=0&sn=demo-device-0001") != 0)
    return 1;
  h2_onenet_t* client = NULL;
  return h2_onenet_destroy(&client) == H2_PAL_OK ? 0 : 1;
}
