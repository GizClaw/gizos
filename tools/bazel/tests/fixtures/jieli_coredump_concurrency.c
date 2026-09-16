#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#undef snprintf
/* Model the SDK formatter symbol using the host formatter, without treating
 * the SDK extern declaration as a fortified host-libc macro. */
int fixture_snprintf(char *out, size_t size, const char *format, ...) {
  va_list args;
  va_start(args,format);
  int result=vsnprintf(out,size,format,args);
  va_end(args);
  return result;
}
#define snprintf fixture_snprintf
#define SEC(x)
#define SYS_RST_WDT 1u
#define SDFILE_SECTOR_SIZE 4096u
#define H2_JIELI_COREDUMP_ADDRESS 0u
#define platform_initcall(x) static int (*keep_init)(void) __attribute__((unused)) = x
struct vfs_attr { unsigned sclust, fsize; };
static int fget_attrs(FILE *f, struct vfs_attr *a) { (void)f; (void)a; return -1; }
static unsigned sdfile_flash_addr2cpu_addr(unsigned a) { return a; }
static int sdfile_reserve_zone_erase(unsigned a, unsigned n, int f) { (void)a; (void)n; (void)f; return 0; }
static unsigned char flash[4096];
static int fault_on_write;
void h2_jieli_wl82_assert_reset_hook(void *caller);
static int sdfile_reserve_zone_write(void *p, unsigned a, size_t n, int f) { (void)f; memcpy(flash+a-1,p,n);
  if (fault_on_write) {
    fault_on_write=0;
    h2_jieli_wl82_assert_reset_hook((void *)2);
  }
  return (int)n; }
static int sdfile_reserve_zone_read(void *p, unsigned a, size_t n, int f) { (void)f; memcpy(p,flash+a-1,n); return (int)n; }
void put_buf(const uint8_t *b, int n) { (void)b; (void)n; }
static uint32_t h2_jieli_atomic_load_u32(volatile uint32_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static void h2_jieli_atomic_store_u32(volatile uint32_t *p,uint32_t v) { __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static _Atomic int hold_writer, writer_entered, release_writer, writer_finished;
static _Thread_local int writer_thread;
int h2_jieli_sdk_try_lock_byte(volatile uint8_t *p) { return !__atomic_exchange_n(p,1,__ATOMIC_ACQUIRE); }
void h2_jieli_sdk_unlock_byte(volatile uint8_t *p) { __atomic_store_n(p,0,__ATOMIC_RELEASE); }
void h2_jieli_sdk_capture_barrier(void) {
  atomic_thread_fence(memory_order_seq_cst);
  if (writer_thread && atomic_load(&hold_writer)) {
    atomic_store(&writer_entered,1);
    while (!atomic_load(&release_writer)) {}
  }
}
/* PROVIDER */
volatile struct h2_jieli_wl82_boot_marker h2_jieli_wl82_ram_marker;
/* Loader build marker; App boot is separately compiled without it. */
#ifdef TEST_LOADER
const uint32_t h2_jieli_wl82_coredump_loader_image = 1u;
#else
const uint32_t h2_jieli_wl82_coredump_loader_image = 0u;
#endif
static void *writer(void *p) {
  (void)p;
  writer_thread=1;
  for (unsigned i=0;i<10000;++i) h2_jieli_wl82_log_byte('a');
  atomic_store(&writer_finished,1);
  return NULL;
}
static void *capture(void *p) {
  (void)p;
  for (unsigned i=0;i<100;++i) h2_jieli_wl82_assert_reset_hook(NULL);
  return NULL;
}
/* WARM_REPORT */
static int warm_lines;
static void warm_emit(const char *line) { if (strstr(line,"H2_JIELI_WARM_LOG")) ++warm_lines; }
/* RECOVERY_POLICY */
int main(int argc,char **argv) {
  assert(argc==2);
  h2_jieli_wl82_reset_recovery_hook(0);
  if (!strcmp(argv[1],"watchdog_loader") || !strcmp(argv[1],"watchdog_app")) {
    retained_log.magic=UINT32_C(0x474f4c48) | (!strcmp(argv[1],"watchdog_loader") ? 1u : 0u);
    h2_jieli_wl82_reset_recovery_hook(SYS_RST_WDT);
    assert(h2_jieli_wl82_take_loader_crash_pending()==!strcmp(argv[1],"watchdog_loader"));
  } else if (!strcmp(argv[1],"early_role")) {
    h2_jieli_wl82_assert_reset_hook(NULL);
#ifdef TEST_LOADER
    assert(h2_jieli_wl82_take_loader_crash_pending()==1);
#else
    assert(h2_jieli_wl82_take_loader_crash_pending()==0);
#endif
  } else if (!strcmp(argv[1],"warm_dirty")) {
    warm_test.magic=SNAPSHOT_MAGIC;
    warm_test.log_magic=UINT32_C(0xc74f4c49);
    warm_test.log_total=1;
    warm_test.log_head=1;
    warm_test.log[0]='x';
    h2_jieli_warm_boot_report(warm_emit);
    assert(warm_lines==0);
  } else if (!strcmp(argv[1],"warm_layout")) {
    assert(RAM_MARKER_ADDR==UINT32_C(0x01c7ed94));
    assert(RETAINED_LOG_ADDR==UINT32_C(0x01c7e588));
  } else if (!strcmp(argv[1],"invalid_pending")) {
    crash_pending=H2_JIELI_CRASH_PENDING;
    crash_origin=H2_JIELI_CRASH_ORIGIN_LOADER;
    memset(&pending_record,0,sizeof(pending_record));
    assert(h2_jieli_wl82_take_loader_crash_pending()==0);
  } else if (!strcmp(argv[1],"recovery_policy")) {
    assert(recovery_policy(SYS_RST_WDT,0)==0);
    assert(recovery_policy(0,1)==1);
  } else if (!strcmp(argv[1],"flush_capture")) {
    coredump_sdfile_addr=1;
    h2_jieli_wl82_assert_reset_hook((void *)1);
    fault_on_write=1;
    assert(h2_jieli_wl82_coredump_flush_pending()==1);
    assert(h2_jieli_record_valid(&pending_record));
    assert(pending_record.caller==2u);
  } else if (!strcmp(argv[1],"preboot")) {
    /* RESET_READY */
    retained_log.magic=UINT32_C(0x474f4c49);
    retained_log.head=0;
    retained_log.total=0;
    h2_jieli_wl82_log_byte('x');
    assert(retained_log.magic==UINT32_C(0x474f4c49));
    assert(retained_log.total==0u);
  } else if (!strcmp(argv[1],"counter")) {
    h2_jieli_wl82_log_byte('x');
    retained_log.total=UINT32_MAX;
    h2_jieli_wl82_log_byte('y');
    h2_jieli_wl82_assert_reset_hook(NULL);
    assert(pending_record.log_bytes==2048u);
    assert(pending_record.log_total==UINT32_MAX);
  } else if (!strcmp(argv[1],"dirty")) {
    retained_log.magic=UINT32_C(0xc74f4c48);
    retained_log.total=1;
    retained_log.head=1;
    retained_log.data[0]='x';
    h2_jieli_wl82_reset_recovery_hook(SYS_RST_WDT);
    assert(h2_jieli_record_valid(&pending_record));
    assert(pending_record.log_bytes==0);
  } else if (!strcmp(argv[1],"busy_log")) {
    h2_jieli_wl82_log_byte('x');
    /* HOLD_LOG */
    h2_jieli_wl82_assert_reset_hook(NULL);
    assert(h2_jieli_record_valid(&pending_record));
    assert(pending_record.log_bytes==0);
    /* RELEASE_LOG */
  } else if (!strcmp(argv[1],"busy_capture")) {
    h2_jieli_wl82_assert_reset_hook((void *)1);
    struct h2_jieli_coredump_record saved=pending_record;
    /* HOLD_CAPTURE */
    h2_jieli_wl82_assert_reset_hook((void *)2);
    assert(!memcmp(&saved,&pending_record,sizeof(saved)));
    /* RELEASE_CAPTURE */
  } else if (!strcmp(argv[1],"threads")) {
    pthread_t a,b,c;
    assert(!pthread_create(&a,NULL,writer,NULL));
    assert(!pthread_create(&b,NULL,capture,NULL));
    assert(!pthread_create(&c,NULL,capture,NULL));
    assert(!pthread_join(a,NULL));
    assert(!pthread_join(b,NULL));
    assert(!pthread_join(c,NULL));
    assert(h2_jieli_record_valid(&pending_record));
  } else if (!strcmp(argv[1],"torn")) {
    pthread_t a;
    atomic_store(&hold_writer,1);
    assert(!pthread_create(&a,NULL,writer,NULL));
    /* WAIT_WRITER */
    h2_jieli_wl82_assert_reset_hook(NULL);
    assert(h2_jieli_record_valid(&pending_record));
    assert(pending_record.log_bytes==0);
    atomic_store(&release_writer,1);
    assert(!pthread_join(a,NULL));
  } else return 2;
  return 0;
}
