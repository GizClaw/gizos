"""Compile the real SDK sys_event section against deterministic SDK doubles."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / 'native_component_src/jieli/wl82/h2_pal_core'
START = '/* ---- System events (SDK sys_event) --------------------------------------- */'
END = "/* Exception capture cannot enter the normal atomic runtime's blocking lock."

STUB = r'''
#include "h2_jieli_wl82_sdk_port.h"
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <string.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef int OS_SEM;
struct sys_event { u16 type; u8 from; u8 len; u8 payload[0]; };
static void (*task_entry)(void *);
static void (*registered)(struct sys_event *);
static int creates, irq, notify_result, notifications, registration_result;
static int create_result, timeout;
static const void *notified;
static jmp_buf task_exit;
static int os_task_create(void (*entry)(void *), void *arg, int prio,
                          int stack, int queue, const char *name) {
    assert(arg == NULL && prio == 20 && stack == 1024 && queue == 32);
    assert(strcmp(name, "h2_sysevt") == 0);
    ++creates;
    task_entry = entry;
    return create_result;
}
static int os_sem_create(OS_SEM *sem, int value) { *sem = value; return 0; }
static int os_sem_post(OS_SEM *sem) { ++*sem; return 0; }
static int os_sem_pend(OS_SEM *sem, int ticks) {
    assert(ticks == 100);
    if (timeout) return -1;
    if (setjmp(task_exit) == 0) task_entry(NULL);
    assert(*sem == 1);
    --*sem;
    return 0;
}
static int os_sem_del(OS_SEM *sem, int flags) { (void)sem; (void)flags; return 0; }
static const char *os_current_task(void) { return "test"; }
static void os_time_dly(int ticks) { (void)ticks; }
static int os_taskq_pend(const char *name, int *msg, int count) {
    assert(strcmp(name, "taskq") == 0 && msg != NULL && count == 8);
    longjmp(task_exit, 1);
    return 0;
}
static int register_sys_event_handler(int type, u8 from, u8 priority,
                                      void (*handler)(struct sys_event *)) {
    assert(type == 0x0100 && from == 0x50 && priority == 0);
    registered = handler;
    return registration_result;
}
static int sys_event_notify(u16 type, u8 from, void *message, u8 size) {
    assert(type == 0x0100 && from == 0x50 && size == 32);
    ++notifications;
    notified = message;
    return notify_result;
}
static int cpu_in_irq(void) { return irq; }
'''

CASES = r'''
static const void *received;
static size_t received_size;
static unsigned deliveries;
static void deliver(const void *message, size_t size) {
    received = message;
    received_size = size;
    ++deliveries;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    /* Some implementations need these SDK helpers only on failure paths. */
    (void)os_sem_del; (void)os_current_task; (void)os_time_dly;
    if (strcmp(argv[1], "create") == 0) {
        create_result = -1;
        assert(h2_jieli_sdk_event_dispatcher_start(deliver) < 0);
        return 0;
    }
    if (strcmp(argv[1], "register") == 0) {
        registration_result = -1;
        assert(h2_jieli_sdk_event_dispatcher_start(deliver) < 0);
        return 0;
    }
    if (strcmp(argv[1], "timeout") == 0) {
        timeout = 1;
        assert(h2_jieli_sdk_event_dispatcher_start(deliver) < 0);
        return 0;
    }
    assert(h2_jieli_sdk_event_dispatcher_start(deliver) == 0);
    assert(creates == 1 && registered != NULL);
    assert(h2_jieli_sdk_event_dispatcher_start(deliver) == 0 && creates == 1);
    struct { struct sys_event event; u8 payload[32]; } message = {
        .event = {.type = 0x0100, .from = 0x50, .len = 32},
    };
    registered(NULL);
    message.event.type = 1; registered(&message.event);
    message.event.type = 0x0100; message.event.from = 1; registered(&message.event);
    message.event.from = 0x50; message.event.len = 31; registered(&message.event);
    assert(deliveries == 0);
    message.event.len = 32; registered(&message.event);
    assert(deliveries == 1 && received == message.event.payload && received_size == 32);
    assert(h2_jieli_sdk_event_post(NULL, 32) == -1);
    assert(h2_jieli_sdk_event_post(message.payload, 31) == -1);
    assert(notifications == 0);
    notify_result = -12;
    assert(h2_jieli_sdk_event_post(message.payload, 32) == 1);
    notify_result = 0;
    assert(h2_jieli_sdk_event_post(message.payload, 32) == 0);
    notify_result = -5;
    assert(h2_jieli_sdk_event_post(message.payload, 32) == -1);
    assert(notifications == 3 && notified == message.payload);
    assert(h2_jieli_sdk_in_interrupt() == 0);
    irq = 1;
    assert(h2_jieli_sdk_in_interrupt() != 0);
    return 0;
}
'''


class SysEventPortTest(unittest.TestCase):
    def test_sdk_contract(self):
        source = (CORE / 'src/h2_jieli_wl82_sdk_port.c').read_text()
        block = source[source.index(START):source.index(END)]
        with tempfile.TemporaryDirectory(prefix='h2-sys-event-') as directory:
            unit = Path(directory) / 'port.c'
            binary = Path(directory) / 'port'
            unit.write_text(STUB + block + CASES)
            subprocess.run([
                *shlex.split(os.environ.get('CC', 'cc')),
                '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                *shlex.split(os.environ.get('JIELI_TEST_CFLAGS', '')),
                '-I', str(CORE / 'include'), str(unit), '-o', str(binary),
            ], check=True, timeout=60)
            for case in ('success', 'create', 'register', 'timeout'):
                with self.subTest(case=case):
                    subprocess.run([str(binary), case], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
