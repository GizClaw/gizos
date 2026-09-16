"""Run both real scan callbacks and entry-task snapshot expressions on pthreads."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
FIXTURE = r'''
#include "h2/pal/hal/h2_pal_ble.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
/* SOURCE */
static scan_state_t scan;
static atomic_uint requested, completed;
static void *producer(void *unused) {
    (void)unused;
    for (unsigned round = 1; round <= 2000; ++round) {
        while (atomic_load_explicit(&requested, memory_order_acquire) != round) sched_yield();
        h2_pal_ble_scan_result_t result = {0};
        result.local_name = "H2PAL";
        result.local_name_len = 5;
        result.rssi = -(int)(round % 100 + 1);
        result.addr.type = (h2_pal_ble_addr_type_t)(round % 2);
        for (unsigned i = 0; i < 6; ++i) result.addr.value[i] = (uint8_t)(round + i);
        assert(!on_scan_result(&scan, &result)); /* Not connectable. */
        result.connectable = true;
        assert(on_scan_result(&scan, &result));
        /* A queued matching report must not rewrite an already published peer. */
        memset(result.addr.value, 0xee, sizeof(result.addr.value));
        result.rssi = -999;
        assert(on_scan_result(&scan, &result));
        atomic_store_explicit(&completed, round, memory_order_release);
    }
    return NULL;
}
static void *consumer(void *unused) {
    (void)unused;
    for (unsigned round = 1; round <= 2000; ++round) {
        atomic_store_explicit(&requested, round, memory_order_release);
        while (/* WAIT */) sched_yield();
        /* SNAPSHOT */
        for (unsigned i = 0; i < 6; ++i) assert(peer_addr.value[i] == (uint8_t)(round + i));
        assert((unsigned)peer_addr.type == round % 2);
        assert(peer_rssi == -(int)(round % 100 + 1));
        while (atomic_load_explicit(&completed, memory_order_acquire) != round) sched_yield();
        /* Models a new scan only after the previous callback has quiesced. */
        scan.found = false;
        memset(&scan.addr, 0, sizeof(scan.addr));
        scan.rssi = 0;
    }
    return NULL;
}
int main(void) {
    pthread_t host, entry;
    assert(pthread_create(&host, NULL, producer, NULL) == 0);
    assert(pthread_create(&entry, NULL, consumer, NULL) == 0);
    assert(pthread_join(host, NULL) == 0);
    assert(pthread_join(entry, NULL) == 0);
    return 0;
}
'''


class PublicationTest(unittest.TestCase):
    def test_callback_to_entry(self):
        for board in ('devkit', 'amoled'):
            with self.subTest(board=board):
                path = f'projects/example/targets/h2loader_tar_zlib/ble-connect-smoke/{board}/main/main.c'
                baseline = os.environ.get('ESP_SCAN_BASELINE')
                source = (subprocess.check_output(['git', 'show', f'{baseline}:{path}'], text=True)
                          if baseline else (ROOT / path).read_text())
                callback = source[source.index('static const' if board == 'devkit' else 'typedef struct scan_state'):
                                  source.index('static void image_entry')]
                wait = re.search(r'rc == H2_PAL_OK && (.*?) &&\s*elapsed <', source).group(1)
                if 'const h2_pal_ble_addr_t peer_addr' in source:
                    snapshot = re.search(r'const h2_pal_ble_addr_t peer_addr = .*?;\s*const int peer_rssi = .*?;', source).group(0)
                    self.assertIn('runtime->ble_host, &peer_addr,', source)
                    self.assertIn('attempt, peer_rssi);', source)
                else:
                    # Historical consumer read these expressions directly at log/connect.
                    addr = re.search(r'runtime->ble_host, &(scan.addr),', source).group(1)
                    rssi = re.search(r'attempt, (scan.rssi)\);', source).group(1)
                    snapshot = f'const h2_pal_ble_addr_t peer_addr = {addr}; const int peer_rssi = {rssi};'
                unit_text = FIXTURE.replace('/* SOURCE */', callback).replace('/* WAIT */', wait).replace('/* SNAPSHOT */', snapshot)
                with tempfile.TemporaryDirectory() as directory:
                    unit = Path(directory) / 'test.c'
                    binary = Path(directory) / 'test'
                    unit.write_text(unit_text)
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                                    '-I', str(ROOT / 'libs/pal/include'),
                                    *shlex.split(os.environ.get('ESP_SCAN_CFLAGS', '')),
                                    str(unit), '-o', str(binary)], check=True)
                    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
