#include "h2_qrcode.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  FILE* file = fopen(argv[1], "rb");
  if (file == NULL) return 1;
  char code[256];
  size_t length = fread(code, 1, sizeof(code), file);
  int error = ferror(file);
  int close_rc = fclose(file);
  if (error || close_rc != 0 || length == 0 || length >= sizeof(code) ||
      memchr(code, '\0', length) != NULL)
    return 1;
  code[length] = '\0';
  uint8_t modules[H2_QRCODE_BUFFER_LEN_FOR_VERSION(12)],
      scratch[sizeof(modules)];
  h2_qrcode_t qr;
  if (h2_qrcode_encode_text(code, H2_QRCODE_ECC_MEDIUM, 12, modules,
                            sizeof(modules), scratch, sizeof(scratch),
                            &qr) != 0)
    return 1;
  unsigned side = (unsigned)qr.size + 8u;
  printf(
      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %u %u\" "
      "width=\"360\" height=\"360\" shape-rendering=\"crispEdges\"><rect "
      "width=\"100%%\" height=\"100%%\" fill=\"white\"/><path fill=\"black\" "
      "d=\"",
      side, side);
  for (int y = 0; y < qr.size; ++y)
    for (int x = 0; x < qr.size; ++x)
      if (h2_qrcode_module_is_dark(&qr, x, y))
        printf("M%d %dh1v1h-1z", x + 4, y + 4);
  printf("\"/></svg>\n");
  return ferror(stdout) ? 1 : 0;
}
