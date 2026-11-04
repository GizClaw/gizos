"""Host regression for the real board UART console with mocked SDK I/O."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "boards/jieli/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_console.c"

STUB = r"""
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
typedef unsigned char u8;
typedef uintptr_t u32;
typedef int OS_MUTEX;
typedef int h2_pal_result_t;
enum { H2_PAL_OK=0, H2_PAL_ERR_IO=-10, H2_PAL_ERR_UNAVAILABLE=-11,
       H2_PAL_ERR_INVALID_ARG=-12, H2_PAL_ERR_TIMEOUT=-13,
       OS_NO_ERR=0, UART_RECV_TIMEOUT=-2, UART_CIRCULAR_BUFFER_WRITE_OVERLAY=-1,
       UART_SET_CIRCULAR_BUFF_ADDR=1, UART_SET_CIRCULAR_BUFF_LENTH,
       UART_SET_RECV_BLOCK, UART_START, UART_FLUSH };
static int handle, opens, closes, creates, locks, unlocks, held, lock_fail;
static int init_fail=1, ioctls, read_result, flushes, writes, short_write;
static unsigned char received[2048];
static size_t received_count;
static int os_mutex_create(OS_MUTEX *m) { (void)m; ++creates; return 0; }
int os_mutex_pend(OS_MUTEX *m, int ticks) {
  (void)m;
  assert(ticks>0); ++locks;
  if (lock_fail) return 1;
  assert(!held); held=1; return 0;
}
static int os_mutex_accept(OS_MUTEX *m) { return os_mutex_pend(m, 1); }
static struct { uint16_t CON0; } uart_registers;
#define JL_UART1 (&uart_registers)
#define BIT(n) (1u << (n))
static uint32_t now;
static uint32_t timer_get_ms(void) { return now++; }
static void os_time_dly(unsigned ticks) { now += ticks * 10u; }
static int os_mutex_post(OS_MUTEX *m) { (void)m; assert(held); held=0; ++unlocks; return 0; }
static void *dev_open(const char *name, void *arg) {
  (void)arg;
  assert(strcmp(name,"uart1")==0); ++opens; return &handle;
}
static int dev_close(void *h) { assert(h==&handle); ++closes; return 0; }
static int dev_ioctl(void *h, int op, u32 arg) {
  assert(h==&handle); ++ioctls;
  if (init_fail) return -1;
  if (op==UART_SET_CIRCULAR_BUFF_LENTH) assert(arg==16*1024);
  if (op==UART_SET_RECV_BLOCK) assert(arg==0);
  if (op==UART_FLUSH) { assert(held); ++flushes; }
  return 0;
}
static int dev_read(void *h, void *data, u32 size) {
  (void)size;
  assert(h==&handle && held);
  if (read_result>0) memset(data,0xab,(size_t)read_result);
  return read_result;
}
static int dev_write(void *h, void *data, u32 size) {
  assert(h==&handle && held && size<=512);
  assert(((uintptr_t)data%32)==0);
  ++writes;
  uart_registers.CON0 = BIT(15);
  if (short_write && size>17) size=17;
  memcpy(received+received_count,data,size); received_count+=size;
  return (int)size;
}
"""

MAIN = r"""
int main(void) {
  unsigned char data[1100], out[32];
  for (int i=0;i<1100;++i) data[i]=(unsigned char)i;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==H2_PAL_ERR_UNAVAILABLE);
  assert(h2_jieli_ac791n_devkit_console_start()==H2_PAL_ERR_IO);
  assert(closes==1 && creates==1);
  init_fail=0;
  assert(h2_jieli_ac791n_devkit_console_start()==0);
  assert(h2_jieli_ac791n_devkit_console_start()==0 && opens==2 && creates==1);
  int before=locks;
  assert(h2_jieli_ac791n_devkit_console_write(data+1,1003,100)==1003);
  assert(locks==before+1 && unlocks==1 && writes==2 && !held);
  assert(memcmp(received,data+1,1003)==0);
  received_count=0; short_write=1;
  assert(h2_jieli_ac791n_devkit_console_write(data,83,100)==83);
  assert(memcmp(received,data,83)==0 && !held);
  lock_fail=1; before=writes;
  assert(h2_jieli_ac791n_devkit_console_write(data,1,0)==H2_PAL_ERR_TIMEOUT);
  assert(writes==before);
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==0 && !held);
  lock_fail=0;
  read_result=UART_RECV_TIMEOUT;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==0);
  read_result=UART_CIRCULAR_BUFFER_WRITE_OVERLAY;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==H2_PAL_ERR_IO && flushes==1);
  read_result=7;
  assert(h2_jieli_ac791n_devkit_console_read(out,32)==7 && out[6]==0xab);
  assert(h2_jieli_ac791n_devkit_console_write(NULL,1,1)==H2_PAL_ERR_INVALID_ARG);
  return 0;
}
"""


class UartConsoleTest(unittest.TestCase):
    def test_app_console_start_retry_keeps_live_transport(self):
        source = (ROOT / "libs/h2loader/device/src/h2_device_serial.c").read_text()
        def function(name):
            begin = source.index(name)
            end = source.index("{", begin) + 1
            depth = 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[begin:end]
        code = function("int h2_device_app_serial_start_with_config(") + function("void h2_device_app_serial_admit_sessions(")
        stub = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
enum { H2_PAL_OK=0,H2_PAL_ERR_INVALID_ARG=-1,H2_PAL_ERR_INVALID_STATE=-2,
 H2_PAL_ERR_NO_MEMORY=-3,H2_ATOMIC_RELEASE=0,H2_SESSION_GATE_FALLBACK_MS=8000,
 H2_APP_COMMAND_STACK_SIZE=49152,H2_ATOMIC_OK=0 };
static const char h2_loader_uart_command_task_name[]= "console";
typedef int h2_loader_app_client_t,h2_pal_task_api_t,h2_pal_mem_api_t;
typedef uint32_t h2_atomic_bool_t;
typedef struct { void *user; int read,write,flush; } h2_iostreamikcp_io_t;
typedef struct { void *user; int *vtable; } h2_command_io_api_t;
typedef struct {
 h2_iostreamikcp_io_t physical_io; const h2_pal_mem_api_t *allocator;
 h2_atomic_bool_t *sessions_admitted,*stop_requested; const void *time; unsigned admission_fallback_ms;
} h2_device_serial_config_t;
typedef struct {
 h2_device_serial_config_t config; h2_iostreamikcp_io_t physical_io;
 const h2_pal_mem_api_t *allocator; const void *time; h2_command_io_api_t io; uint32_t gate_started_ms;
} h2_device_serial_t;
typedef struct { h2_device_serial_config_t transport; const h2_pal_task_api_t *task; const char *task_name; size_t stack_size; } h2_device_app_serial_config_t;
typedef struct {
 h2_device_serial_t transport; h2_command_io_api_t io;
 h2_loader_app_client_t *client; const h2_pal_task_api_t *task; h2_atomic_bool_t stop_requested,sessions_admitted; int admission_requested,initialized,started;
} h2_device_app_serial_t;
typedef struct {
 h2_loader_app_client_t *client; const h2_pal_task_api_t *task;
 void *read_user,*write_user; int read_byte,write;
 const char *task_name; size_t stack_size;
} h2_loader_app_client_return_console_config_t;
static h2_device_app_serial_t state;
static int h2_atomic_bool_init(h2_atomic_bool_t *value,bool initial) { *value=initial;return H2_ATOMIC_OK; }
static void h2_atomic_bool_destroy(h2_atomic_bool_t *value) { *value=0; }
static int app_read_byte,app_write,transport_inits,command_calls,command_fail;
static uint32_t now_ms_value=1234;
static void h2_atomic_bool_store(h2_atomic_bool_t *value,bool admitted,int order) {
 (void)order;*value=admitted;
}
static int h2_device_serial_init_with_config(h2_device_serial_t *transport,const h2_device_serial_config_t *config) {
 ++transport_inits;transport->config=*config;transport->physical_io=config->physical_io;
 transport->allocator=config->allocator;transport->time=config->time;return 0;
}
static h2_command_io_api_t h2_device_serial_io(h2_device_serial_t *transport) { return (h2_command_io_api_t){.user=transport}; }
static uint32_t h2_device_serial_now(h2_device_serial_t *transport) { (void)transport;return now_ms_value; }
static int h2_loader_app_client_start_return_console(const h2_loader_app_client_return_console_config_t *config) {
 assert(config->read_user==&state && config->write_user==&state && state.started && state.initialized);
 assert(config->stack_size==49152);++command_calls;return command_fail?H2_PAL_ERR_NO_MEMORY:0;
}
'''
        main = r'''
int main(void) {
 h2_loader_app_client_t client=0,other_client=0;h2_pal_task_api_t task=0;
 h2_pal_mem_api_t allocator=0,other_allocator=0;
 h2_device_app_serial_config_t config={.task=&task,.stack_size=49152,.transport={.admission_fallback_ms=8000,.allocator=&allocator,.physical_io={.user=&task,.read=1,.write=2}}};
 h2_device_app_serial_admit_sessions(&state);assert(state.admission_requested==1);
 command_fail=1;
 assert(h2_device_app_serial_start_with_config(&state,&client,&config)==H2_PAL_ERR_NO_MEMORY);
 assert(state.initialized && !state.started && transport_inits==1 && state.sessions_admitted==1);
 assert(h2_device_app_serial_start_with_config(&state,&other_client,&config)==H2_PAL_ERR_INVALID_STATE);
 config.transport.allocator=&other_allocator;
 assert(h2_device_app_serial_start_with_config(&state,&client,&config)==H2_PAL_ERR_INVALID_STATE);
 config.transport.allocator=&allocator;
 assert(h2_device_app_serial_start_with_config(&state,&client,&config)==H2_PAL_ERR_NO_MEMORY);
 assert(transport_inits==1 && command_calls==2 && state.sessions_admitted==1);
 command_fail=0;now_ms_value=5678;
 assert(h2_device_app_serial_start_with_config(&state,&client,&config)==0);
 assert(state.started && state.transport.gate_started_ms==5678 && transport_inits==1 && state.sessions_admitted==1);
 assert(state.transport.config.sessions_admitted==&state.sessions_admitted && state.transport.config.admission_fallback_ms==8000);
 assert(h2_device_app_serial_start_with_config(&state,&client,&config)==H2_PAL_ERR_INVALID_STATE && command_calls==3);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            unit.write_text(stub + code + main)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(unit), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_app_console_sync_create_failure_unwinds(self):
        source = (ROOT / "native_component_src/jieli/wl82/h2_pal_image/src/h2_jieli_wl82_console.c").read_text()
        begin = source.index("static int console_sync_init(")
        end = source.index("int h2_jieli_wl82_console_start(", begin)
        stub = r'''
#include <assert.h>
#include <string.h>
enum { OS_NO_ERR=0, H2_PAL_OK=0, H2_PAL_ERR_NO_MEMORY=-1 };
typedef struct {
  int rx_mutex, tx_mutex, rx_wakeup_sem, rx_data_sem;
} h2_jieli_app_console_t;
static int attempt, fail_at, live, deleted, deletion_order[4];
static int create(int *object) {
  ++attempt;
  assert(*object == 0);
  if (attempt == fail_at) return -1;
  *object = attempt; ++live; return 0;
}
static int os_mutex_create(int *object) { return create(object); }
static int os_sem_create(int *object, int count) {
  assert(count == 0); return create(object);
}
static int destroy(int *object, int force) {
  assert(*object != 0 && force == 0);
  deletion_order[deleted++] = *object;
  *object = 0; --live; return 0;
}
static int os_mutex_del(int *object, int force) { return destroy(object, force); }
static int os_sem_del(int *object, int force) { return destroy(object, force); }
'''
        main = r'''
int main(void) {
  h2_jieli_app_console_t state = {0};
  for (fail_at = 1; fail_at <= 4; ++fail_at) {
    attempt = deleted = 0;
    assert(console_sync_init(&state) == H2_PAL_ERR_NO_MEMORY);
    assert(attempt == fail_at && live == 0 && deleted == fail_at - 1);
    for (int i = 0; i < deleted; ++i) {
      assert(deletion_order[i] == fail_at - 1 - i);
    }
  }
  attempt = deleted = fail_at = 0;
  assert(console_sync_init(&state) == H2_PAL_OK);
  assert(live == 4 && attempt == 4 && deleted == 0);
  console_sync_destroy(&state);
  assert(live == 0 && deleted == 4);
  for (int i = 0; i < deleted; ++i) assert(deletion_order[i] == 4 - i);
  return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-app-sync-test-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(stub + source[begin:end].replace("h2_jieli_console_t", "h2_jieli_app_console_t") + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)

    def test_native_console_start_retry_preserves_live_rx_storage(self):
        source = (ROOT / "native_component_src/jieli/wl82/h2_pal_image/src/h2_jieli_wl82_console.c").read_text()
        code = source[source.index("static int console_sync_init("):source.index("void h2_jieli_wl82_diag_write(")]
        stub = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
enum { OS_NO_ERR=0,H2_PAL_OK=0,H2_PAL_ERR_NO_MEMORY=-1,H2_PAL_ERR_IO=-2,H2_PAL_ERR_BUSY=-3,
 DEBUG_RAW_IDLE=0,DEBUG_RAW_BUSY,DEBUG_STARTING,DEBUG_READY,H2_USB_RX_TASK_STACK_SIZE=4096,USB_MAX_HW_NUM=2 };
typedef unsigned usb_dev;
typedef struct {
 int rx_mutex,tx_mutex,rx_wakeup_sem,rx_data_sem,initialized,started;
 unsigned usb_id;uint8_t rx_ring[8192];size_t rx_count;
} h2_jieli_console_t;
static h2_jieli_console_t state;
static volatile uint32_t debug_gate=DEBUG_RAW_IDLE;
static int creates,live,board_calls,board_fail,worker_fail,worker_count,handlers,raw_waits;
static void h2_jieli_atomic_store_u32(volatile uint32_t *p,uint32_t v) { *p=v; }
static int h2_jieli_atomic_cas_u32(volatile uint32_t *p,uint32_t *expected,uint32_t v) {
 if(*p!=*expected) { *expected=*p;return 0; } *p=v;return 1;
}
static void os_time_dly(unsigned ticks) {
 assert(ticks==1 && debug_gate==DEBUG_RAW_BUSY && creates==0);++raw_waits;debug_gate=DEBUG_RAW_IDLE;
}
static int os_mutex_create(int *object) { assert(*object==0);*object=++creates;++live;return 0; }
static int os_sem_create(int *object,int n) { assert(n==0);return os_mutex_create(object); }
static int os_mutex_del(int *object,int force) {
 assert(force==0 && *object!=0 && worker_count==0 && handlers==0);*object=0;--live;return 0;
}
static int os_sem_del(int *object,int force) { return os_mutex_del(object,force); }
static int board_start(void) { ++board_calls;assert(live==4);return board_fail?H2_PAL_ERR_IO:0; }
typedef struct { int (*start)(void); } h2_jieli_wl82_console_config_t;
static const h2_jieli_wl82_console_config_t *h2_jieli_wl82_console_board_config(void) {
 static const h2_jieli_wl82_console_config_t config={board_start};return &config;
}
#ifndef CONFIG_H2_UART1_DEBUG_ENABLE
static void usb_rx_task(void *user) { (void)user; }
static void usb_cdc_wakeup(void *device) { (void)device; }
static int os_task_create(void (*entry)(void *),void *user,int priority,int stack,int queue,const char *name) {
 assert(entry==usb_rx_task && user==&state && priority==12 && stack==4096 && queue==0);
 assert(strcmp(name,"h2console_rx")==0 && live==4 && handlers==0 && worker_count==0);
 if(worker_fail)return -1;++worker_count;return 0;
}
static void cdc_set_wakeup_handler(usb_dev id,void (*handler)(void *)) {
 (void)id;assert(handler==usb_cdc_wakeup && worker_count==1 && live==4);++handlers;
}
#endif
'''
        main = r'''
int main(void) {
 debug_gate=DEBUG_RAW_BUSY;board_fail=1;
 assert(h2_jieli_wl82_console_start()==H2_PAL_ERR_IO && raw_waits==1 && live==0 && debug_gate==DEBUG_RAW_IDLE);
 board_fail=0;
#ifndef CONFIG_H2_UART1_DEBUG_ENABLE
 worker_fail=1;
 assert(h2_jieli_wl82_console_start()==H2_PAL_ERR_NO_MEMORY && live==0 && !state.initialized && handlers==0 && debug_gate==DEBUG_RAW_IDLE);
#endif
 worker_fail=0;
 assert(h2_jieli_wl82_console_start()==0 && state.initialized && state.started && live==4 && debug_gate==DEBUG_READY);
 int saved_creates=creates,saved_board_calls=board_calls,saved_mutex=state.rx_mutex;
 state.usb_id=1;state.rx_ring[0]=0x42;state.rx_count=1;
 assert(h2_jieli_wl82_console_start()==0 && state.usb_id==1 && state.rx_ring[0]==0x42 && state.rx_count==1 && state.rx_mutex==saved_mutex);
 assert(creates==saved_creates && board_calls==saved_board_calls && live==4);
#ifndef CONFIG_H2_UART1_DEBUG_ENABLE
 assert(worker_count==1 && handlers==USB_MAX_HW_NUM);
#endif
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            unit = Path(directory) / "test.c"
            unit.write_text(stub + code + main)
            for uart in (False, True):
                with self.subTest(uart=uart):
                    binary = Path(directory) / ("uart" if uart else "usb")
                    flags = ["-DCONFIG_H2_UART1_DEBUG_ENABLE=1"] if uart else []
                    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags, str(unit), "-o", str(binary)], check=True)
                    subprocess.run([str(binary)], check=True, timeout=10)

    def test_sdk_sequence_and_atomic_writes(self):
        source = "\n".join(line for line in SOURCE.read_text().splitlines()
                           if not line.startswith("#include"))
        with tempfile.TemporaryDirectory(prefix="h2-uart-test-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(STUB + source + MAIN)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
