#include "h2_onenet.h"

int main() {
  h2_onenet_t* client = nullptr;
  return h2_onenet_destroy(&client) == H2_PAL_OK &&
                 !h2_onenet_is_connected(client)
             ? 0
             : 1;
}
