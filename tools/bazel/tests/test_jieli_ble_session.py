"""Compile the real App session handler; inject a retained console task on join failure."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "projects/h2loader/native_component_src/jieli/wl82/h2loader_app/src/jieli_app_ble.c"

class SessionTest(unittest.TestCase):
    def test_registered_server_stack(self):
        source = (ROOT / "native_component_src/jieli/wl82/h2_pal_core/src/h2_jieli_wl82_sdk_port.c").read_text()
        body = source[source.index("int h2_jieli_sdk_task_create("):source.index("int h2_jieli_sdk_task_delete(")]
        for target in ("display", "audio-system", "touch", "button"):
            policy = (ROOT / f"projects/example/targets/h2loader_tar_zlib/{target}/jieli_ac791n_devkit/BUILD.bazel").read_text()
            words = re.search(r'"\$bleikcp/server \d+ (\d+) \d+"', policy).group(1)
            fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#define OS_NO_ERR 0
struct task_info { const char *name; unsigned prio; u32 stack_size; unsigned qsize; };
const struct task_info h2_jieli_default_task_policy = {"default", 10, 4096, 128};
static size_t actual_stack;
static int task_create(void (*entry)(void *), void *ctx, const char *name) {
 (void)entry;
 (void)ctx;
 assert(strcmp(name, "$bleikcp/server") == 0);
 actual_stack = POLICY_WORDS * 4u;
 return 0;
}
static int os_task_create(void (*entry)(void *), void *ctx, unsigned p, u32 s, int q, const char *n) {
 (void)entry;
 (void)ctx;
 (void)p;
 (void)s;
 (void)q;
 (void)n;
 assert(0);
 return -1;
}
static void entry(void *ctx) { (void)ctx; }
'''
            main = r'''
int main(void) {
 assert(h2_jieli_sdk_task_create(entry, NULL, "$bleikcp/server", 49152u) == 0);
 assert(actual_stack >= 49152u);
 return 0;
}
'''
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                path = Path(directory)
                (path / "test.c").write_text(f"#define POLICY_WORDS {words}u\n" + fixture + body + main)
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
                result = subprocess.run([str(path / "test")], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_session_does_not_return_with_borrowers(self):
        source = SOURCE.read_text()
        handler = source[source.index("static int handle_session("):source.index("int h2_jieli_app_loader_ble_start(")]
        stub = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
enum { H2_PAL_OK=0, H2_PAL_ERR_IO=-8 };
typedef int h2_bleikcp_t;
typedef int h2_loader_app_client_t;
typedef struct {
 h2_loader_app_client_t *client;
 const void *task;
 void *read_user;
 int (*read_byte)(void *, uint32_t);
 void *write_user;
 int (*write)(void *, const char *, size_t);
 const char *task_name;
 size_t stack_size;
} h2_loader_app_client_return_console_config_t;
#define H2LOADER_BLE_COMMAND_TASK_NAME_VALUE "console"
static int borrowers, reads;
int h2_loader_app_client_init(h2_loader_app_client_t *c, void *u) {
 (void)u;
 *c=123;
 return 0;
}
const void *h2_jieli_wl82_platform_task_api(void) { return NULL; }
int h2_loader_ble_app_read_byte(void *u, uint32_t ms) {
 (void)ms;
 assert(*(h2_bleikcp_t *)u==456);
 ++reads;
 return -3;
}
int h2_loader_ble_app_write(void *u, const char *s, size_t n) {
 (void)u;
 (void)s;
 (void)n;
 return 0;
}
int h2_loader_app_client_start_return_console(const h2_loader_app_client_return_console_config_t *c) {
 assert(*c->client==123);
 ++borrowers;
 return 0;
}
int h2_loader_app_client_join_return_console(h2_loader_app_client_t *c) {
 (void)c;
 return H2_PAL_ERR_IO;
}
int h2_loader_app_client_run_return_console(const h2_loader_app_client_return_console_config_t *c) {
 assert(*c->client==123);
 assert(c->read_byte(c->read_user,50)==-3);
 return 0;
}
'''
        main = r'''
int main(void) {
 h2_bleikcp_t stream=456;
 int rc=handle_session(NULL,&stream,1);
 assert(borrowers==0);
 assert(rc==H2_PAL_OK);
 assert(reads==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(stub + handler + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path / "test.c"), "-o", str(path / "test")], check=True)
            result = subprocess.run([str(path / "test")], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

if __name__ == "__main__":
    unittest.main()
