"""Execute the TIMER5 reader/ISR against register and lock test doubles."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ClockDriverTest(unittest.TestCase):
    def test_start_pending_overflow_and_concurrent_readers(self):
        source = (ROOT / "boards/jieli_ac791n_devkit/ac791n/src/h2_jieli_ac791n_devkit_clock.c").read_text()
        body = source[source.index("static DEFINE_SPINLOCK"):source.rindex("#endif")]
        fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include "h2/pal/core/h2_pal_errors.h"
#include "h2_jieli_ac791n_clock_math.h"
#define BIT(n) (1u<<(n))
#define ___interrupt
#define SEC_USED(section)
#define DEFINE_SPINLOCK(name) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
#define spin_acquire(lock) assert(pthread_mutex_lock(lock)==0)
#define spin_release(lock) assert(pthread_mutex_unlock(lock)==0)
static _Thread_local unsigned irq_enabled=1;
#define local_irq_save(flags) do { flags=irq_enabled; irq_enabled=0; } while(0)
#define local_irq_restore(flags) do { irq_enabled=flags; } while(0)
static struct { uint32_t CON,CNT,PRD; } registers;
#define JL_TIMER5 (&registers)
#define IRQ_TIMER5_IDX 9
static int frequency=24000000, registrations;
static int clk_get(const char *name) { assert(!strcmp(name,"osc")); return frequency; }
static void request_irq(unsigned id, unsigned priority, void (*handler)(void), unsigned cpu) {
 assert(id==IRQ_TIMER5_IDX && priority==1 && handler && cpu==0); ++registrations;
}
'''
        main = r'''
static void *reader(void *unused) {
 (void)unused;
 uint64_t previous=0, us;
 for(unsigned i=0;i<10000;++i) {
  assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==H2_PAL_OK);
  assert(us>=previous && irq_enabled==1); previous=us;
 }
 return NULL;
}
static void *interrupts(void *unused) {
 (void)unused;
 /* Keeping the fake pending bit set models a sequence of overflow events;
  * real hardware acknowledges it with the W1C bit. Access is serialized. */
 for(unsigned i=0;i<10000;++i) clock_overflow();
 return NULL;
}
int main(void) {
 uint64_t us=123;
 assert(h2_jieli_ac791n_devkit_clock_read_us(NULL)==H2_PAL_ERR_INVALID_ARG);
 frequency=1;
 assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==H2_PAL_ERR_UNAVAILABLE);
 assert(us==123 && irq_enabled==1 && registrations==0);
 frequency=24000000; registers.CON=1;
 assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==H2_PAL_ERR_UNAVAILABLE);
 registers.CON=0;
 assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==0 && us==0);
 assert(registrations==1 && registers.PRD==65535);
 assert(registers.CON==((14u<<4)|BIT(3)|BIT(0)));
 registers.CNT=1;
 assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==0 && us==341);
 registers.CNT=0; registers.CON|=BIT(15);
 assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==0);
 uint64_t pending=us;
 clock_overflow();
 registers.CON&=~BIT(15); /* Model the W1C acknowledgement. */
 assert(h2_jieli_ac791n_devkit_clock_read_us(&us)==0 && us==pending);
 assert(completed_ticks==65535 && registrations==1);
 registers.CON|=BIT(15);
 pthread_t a,b,c;
 assert(!pthread_create(&a,NULL,reader,NULL));
 assert(!pthread_create(&b,NULL,reader,NULL));
 assert(!pthread_create(&c,NULL,interrupts,NULL));
 assert(!pthread_join(a,NULL) && !pthread_join(b,NULL) && !pthread_join(c,NULL));
 assert(completed_ticks==UINT64_C(10001)*65535);
}
'''
        with tempfile.TemporaryDirectory(prefix="h2-clock-driver-") as directory:
            test = Path(directory) / "test.c"
            test.write_text(fixture + body + main)
            binary = Path(directory) / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-fsanitize=undefined", "-I", str(ROOT / "libs/pal/include"),
                            "-I", str(ROOT / "boards/jieli_ac791n_devkit/ac791n/include"),
                            str(test), "-o", str(binary)], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
