"""Exercise SDK dispatch and byte-to-word sizing with a target default."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DynamicTaskTest(unittest.TestCase):
    def test_target_policy_dispatch(self):
        source = (ROOT / "native_component_src/jieli/wl82/h2_pal_core/src/h2_jieli_wl82_sdk_port.c").read_text()
        body = source[source.index("int h2_jieli_sdk_task_create("):source.index("int h2_jieli_sdk_task_delete(")]
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
#define OS_NO_ERR 0
struct task_info { const char *name; unsigned prio; u32 stack_size; unsigned qsize; };
const struct task_info h2_jieli_default_task_policy = {"default", 13, 2048, 77};
static int registered_calls, dynamic_calls, fail_create;
static u32 words;
static int task_create(void (*entry)(void *), void *ctx, const char *name) {
  (void)entry; (void)ctx; assert(!strcmp(name,"registered")); registered_calls++; return 0;
}
static int os_task_create(void (*entry)(void *),void *ctx,unsigned priority,u32 stack,int queue,const char *name) {
  assert(entry && ctx && priority==13 && queue==77 && !strncmp(name,"$h2anon/",8));
  words=stack; dynamic_calls++; return fail_create;
}
static void entry(void *ctx) { (void)ctx; }
'''
        main = r'''
int main(void) {
 int ctx=0;
 assert(h2_jieli_sdk_task_create(entry,&ctx,"registered",4096)==0);
 assert(registered_calls==1 && dynamic_calls==0);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"$h2anon/1",4096)==0 && words==2048);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"$h2anon/2",8193)==0 && words==2049);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"$h2anon/3",SIZE_MAX)==-1);
 assert(dynamic_calls==2);
 fail_create=1;
 assert(h2_jieli_sdk_task_create(entry,&ctx,"$h2anon/4",4096)==-1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "test.c").write_text(fixture + body + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(root / "test.c"), "-o", str(root / "test")],
                           check=True, timeout=60)
            subprocess.run([str(root / "test")], check=True, timeout=10)
