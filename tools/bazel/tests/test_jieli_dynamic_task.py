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
struct task_info { const char *name; unsigned prio; u32 stack_size; unsigned qsize; void *tcb_stk_q; };
const struct task_info h2_jieli_default_task_policy = {"default", 13, 2048, 77, NULL};
const struct task_info task_info_table[] = {
 {"registered", 17, 1024, 55, NULL},
 {"#C0pinned", 18, 4096, 66, NULL},
 {"#C1other", 19, 2048, 44, NULL},
 {"static", 17, 1024, 55, (void *)1},
 {NULL, 0, 0, 0, NULL},
};
static int registered_calls, dynamic_calls, fail_create, pinned_calls;
static u32 words;
static int os_task_create(void (*entry)(void *), void *ctx, unsigned priority, u32 stack, int queue, const char *name) {
 assert(entry && ctx);
 if (strncmp(name, "registered/", 11) == 0) {
  assert(priority == 17 && queue == 55);
  ++registered_calls;
 } else if (!strcmp(name, "#C0pinned/1")) {
  assert(priority == 18 && queue == 66);
  ++pinned_calls;
 } else if (!strcmp(name, "#C1other/1")) {
  assert(priority == 19 && queue == 44);
  ++pinned_calls;
 } else {
  assert(priority == 13 && queue == 77 && !strncmp(name, "$h2anon/", 8));
  ++dynamic_calls;
 }
 words = stack;
 return fail_create;
}
static void entry(void *ctx) { (void)ctx; }
'''
        main = r'''
int main(void) {
 int ctx=0;
 assert(h2_jieli_sdk_task_create(entry,&ctx,"registered","registered/1",4096)==0);
 assert(registered_calls==1 && dynamic_calls==0 && words==1024);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"registered","registered/2",8193)==0 && words==2049);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"missing","missing/1",4096)==-1);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"static","static/1",4096)==-1);
 assert(h2_jieli_sdk_task_create(entry,&ctx,NULL,"$h2anon/1",4096)==0 && words==2048);
 assert(h2_jieli_sdk_task_create(entry,&ctx,NULL,"$h2anon/2",8193)==0 && words==2049);
 assert(h2_jieli_sdk_task_create(entry,&ctx,NULL,"$h2anon/3",SIZE_MAX)==-1);
 assert(dynamic_calls==2);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"pinned","pinned/1",4096)==0 && words==4096);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"#C0pinned","pinned/1",4096)==0 && words==4096);
 assert(h2_jieli_sdk_task_create(entry,&ctx,"other","other/1",4096)==0 && words==2048);
 assert(pinned_calls==3);
 fail_create=1;
 assert(h2_jieli_sdk_task_create(entry,&ctx,NULL,"$h2anon/4",4096)==-1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "test.c").write_text(fixture + body + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(root / "test.c"), "-o", str(root / "test")],
                           check=True, timeout=60)
            subprocess.run([str(root / "test")], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
