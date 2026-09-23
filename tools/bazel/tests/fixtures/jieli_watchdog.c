#include <assert.h>
#include <pthread.h>
#include "h2_atomic.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "h2_jieli_wl82_atomic.h"
#define SEC(x)
#define CONFIG_H2_WATCHDOG_ENABLE 1
static _Thread_local unsigned cpu;
static h2_atomic_uint_t feeds;
static unsigned wdt_con;
static __attribute__((unused)) unsigned current_cpu_id(void) { return cpu; }
int h2_jieli_sdk_try_lock_byte(volatile uint8_t *p) { return !__atomic_exchange_n(p,1,__ATOMIC_ACQUIRE); }
void h2_jieli_sdk_unlock_byte(volatile uint8_t *p) { __atomic_store_n(p,0,__ATOMIC_RELEASE); }
static void p33_or_1byte(unsigned address, unsigned value) {
  assert(address==0x80 && value==0x40);
  h2_atomic_fetch_add(&feeds,1);
}
static void wdt_init(unsigned time) { wdt_con=(time&15)|0x30; }
static __attribute__((unused)) void wdt_reset_enable(void) { wdt_con=(wdt_con&~0x60u)|0x40u; }
static __attribute__((unused)) void wdt_close(void) { wdt_con=0; }
/* PROVIDER */
/* BOOT */
static void *runner(void *arg) {
  cpu=(unsigned)(uintptr_t)arg;
  for (unsigned i=0;i<10000;++i) wdt_clear();
  return NULL;
}
static void h2_fixture_atomic_cleanup(void) {
    h2_atomic_destroy(&feeds);
}
int main(int argc,char **argv) {
    assert(atexit(h2_fixture_atomic_cleanup) == 0);
    assert(h2_atomic_init(&feeds, 0) == H2_ATOMIC_OK);

  assert(argc==2);
  if (!strcmp(argv[1],"boot")) {
    setup_arch();
    assert((wdt_con&0x1f)==0x1c);
    assert(!(wdt_con&0x20));
  } else if (!strcmp(argv[1],"stopped_0") || !strcmp(argv[1],"stopped_1")) {
    unsigned live=!strcmp(argv[1],"stopped_0") ? 1u : 0u;
    cpu=1u-live;
    wdt_clear();
    runner((void *)(uintptr_t)live);
    assert(h2_atomic_load(&feeds)<=1u);
  } else if (!strcmp(argv[1],"healthy")) {
    for (unsigned i=0;i<1000;++i) {
      cpu=0;
      wdt_clear();
      cpu=1;
      wdt_clear();
    }
    assert(h2_atomic_load(&feeds)==1000u);
  } else if (!strcmp(argv[1],"threads")) {
    pthread_t a,b;
    assert(!pthread_create(&a,NULL,runner,(void *)0));
    assert(!pthread_create(&b,NULL,runner,(void *)1));
    assert(!pthread_join(a,NULL));
    assert(!pthread_join(b,NULL));
    unsigned before=h2_atomic_load(&feeds);
    runner((void *)1);
    assert(h2_atomic_load(&feeds)-before<=1u);
  } else return 2;
  return 0;
}
