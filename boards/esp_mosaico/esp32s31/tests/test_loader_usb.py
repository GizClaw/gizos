"""Exercise production USB callbacks with bounded partial-I/O SDK faults."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class LoaderUsbTest(unittest.TestCase):
    def test_configuration_is_startup_only(self):
        root = Path(__file__).resolve().parents[4]
        path = root / "projects/h2loader/native_component_src/esp-idf6.x/h2_h2loader_runtime/src/h2_esp_h2loader_iostreamikcp.c"
        source = path.read_text()
        source = source[source.index("static int s_console_initialized;"):source.index("static uint32_t transport_now_ms")]
        prefix = '#include <assert.h>\n#include "h2_iostreamikcp_types.h"\n'
        checks = r"""
static int rd(void *u, void *b, size_t n, size_t *o, uint32_t t) {
 (void)u; (void)b; (void)n; (void)o; (void)t; return 0;
}
static int wr(void *u, const void *b, size_t n, size_t *o, uint32_t t) {
 (void)u; (void)b; (void)n; (void)o; (void)t; return 0;
}
int main(void) {
 h2_iostreamikcp_io_t io = {.read=rd, .write=wr, .user=&io};
 assert(h2_esp_h2loader_configure_physical_io(0) == H2_PAL_ERR_INVALID_ARG);
 h2_iostreamikcp_io_t incomplete = {.read=rd};
 assert(h2_esp_h2loader_configure_physical_io(&incomplete) == H2_PAL_ERR_INVALID_ARG);
 s_console_initialized = 1;
 assert(h2_esp_h2loader_configure_physical_io(&io) == H2_PAL_ERR_INVALID_STATE);
 s_console_initialized = 0;
 assert(h2_esp_h2loader_configure_physical_io(&io) == 0);
 assert(s_physical_io.read == rd && s_physical_io.write == wr && s_physical_io.user == &io);
 assert(h2_esp_h2loader_configure_physical_io(&io) == H2_PAL_ERR_INVALID_STATE);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "test.c", Path(tmp) / "test"
            src.write_text(prefix + source + checks)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(root / "libs/pal/include"),
                            "-I" + str(root / "libs/iostreamikcp/include"),
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_faults(self):
        root = Path(__file__).resolve().parents[4]
        source = (root / "boards/esp_mosaico/esp32s31/loader_usb/transport.c").read_text()
        # Replace only SDK includes; compile the actual callback implementation.
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        prefix = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2_iostreamikcp_types.h"
#define CONFIG_TINYUSB_CDC_COUNT 2
#define TINYUSB_CDC_ACM_1 1
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_NOT_FINISHED 2
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef struct { int cdc_port; } tinyusb_config_cdcacm_t;
static int64_t now;
static int rx_ready, rx_error, flush_result, console_error, init_error;
static size_t chunk = 3, sent;
static h2_iostreamikcp_io_t configured;
static int64_t esp_timer_get_time(void) { return now; }
static void vTaskDelay(int ticks) { now += ticks * 1000; }
static int tinyusb_cdcacm_read(int port, void *buf, size_t len, size_t *out) {
 assert(port == 1); *out = 0;
 if (rx_error) return -1;
 if (rx_ready && len) { *(char *)buf = 'x'; *out = 1; }
 return 0;
}
static size_t tinyusb_cdcacm_write_queue(int port,const uint8_t *buf,size_t len) {
 assert(port == 1 && buf); size_t n = len < chunk ? len : chunk; sent += n; return n;
}
static int tinyusb_cdcacm_write_flush(int port, unsigned ticks) {
 assert(port == 1); (void)ticks; return flush_result;
}
static int h2_mosaico_usb_console_init(void) { return console_error; }
static int tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *cfg) {
 assert(cfg->cdc_port == 1); return init_error;
}
static int h2_esp_h2loader_configure_physical_io(const h2_iostreamikcp_io_t *io) {
 configured = *io; return 0;
}
"""
        checks = r"""
int main(void) {
 char bytes[10] = {0}; size_t n = 99;
 assert(h2_mosaico_loader_usb_init() == 0 && configured.read && configured.write);
 assert(configured.read(0, bytes, sizeof(bytes), &n, 3) == H2_PAL_ERR_TIMEOUT);
 assert(n == 0 && now == 3000);
 rx_ready = 1; assert(configured.read(0, bytes, 10, &n, 0) == 0 && n == 1);
 rx_error = 1; assert(configured.read(0, bytes, 10, &n, 0) == H2_PAL_ERR_IO);
 flush_result = ESP_ERR_NOT_FINISHED;
 assert(configured.write(0, bytes, 10, &n, 10) == 0 && n == 10 && sent == 10);
 chunk = 0; int64_t before = now;
 assert(configured.write(0, bytes, 10, &n, 2) == H2_PAL_ERR_TIMEOUT);
 assert(n == 0 && now - before == 2000);
 chunk = 3; flush_result = -1;
 assert(configured.write(0, bytes, 10, &n, 2) == H2_PAL_ERR_IO && n == 3);
 flush_result = ESP_ERR_TIMEOUT; assert(configured.flush(0) == H2_PAL_ERR_TIMEOUT);
 flush_result = ESP_OK; assert(configured.flush(0) == 0);
 console_error = -1; assert(h2_mosaico_loader_usb_init() == H2_PAL_ERR_IO);
 console_error = 0; init_error = -1; assert(h2_mosaico_loader_usb_init() == H2_PAL_ERR_IO);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "test.c"
            exe = Path(tmp) / "test"
            src.write_text(prefix + source + checks)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(root / "libs/pal/include"),
                            "-I" + str(root / "libs/iostreamikcp/include"),
                            str(src), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
