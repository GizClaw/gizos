#include "h2_lingxi.h"

int main() {
  h2_lingxi_device_info_t info{};
  h2_lingxi_device_info_clear(&info);
  h2_lingxi_device_info_clear(nullptr);
  return info.device_id[0] == '\0' ? 0 : 1;
}
