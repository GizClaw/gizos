#include "h2_gizclaw_e2e_devkit_capture.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static unsigned writes, drains;
static int write_error, drain_error;
static bool short_write;
static char delivered[512];
static int write_record(void *user, const void *bytes, size_t size,
                        size_t *written, uint32_t timeout) {
  (void)user;
  assert(timeout == 5000u && size < sizeof(delivered));
  ++writes;
  *written = short_write ? size - 1u : size;
  memcpy(delivered, bytes, *written);
  delivered[*written] = '\0';
  return write_error;
}
static int drain(void *user) { (void)user; ++drains; return drain_error; }
static const h2_pal_uart_io_stream_vtable_t uart_ops = {
    .write = write_record, .flush = drain};
static const h2_pal_uart_io_stream_api_t uart = {.vtable = &uart_ops};
static const char ready[] = "H2_GIZCLAW_E2E stage=ble-binding-ready window=h2e2e-0123456789abcdef revision=42 url_bytes=112 url_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n";

static unsigned random_calls;
static int random_error;
static bool zero_random;
static int random_bytes(void *user, uint8_t *out, size_t size) {
  (void)user;
  ++random_calls;
  assert(size == 16u);
  for (size_t i = 0u; i < size; ++i) out[i] = zero_random ? 0u : (uint8_t)(i + 1u);
  return random_error;
}
static const h2_pal_crypto_vtable_t crypto_ops = {.random = random_bytes};
static const h2_pal_crypto_api_t crypto = {.vtable = &crypto_ops};

static unsigned startup_waits;
static int sleep_error;
static int startup_sleep(void *user, uint32_t ms) {
  (void)user;
  assert(ms == 5000u);
  ++startup_waits;
  return sleep_error;
}
static const h2_pal_time_vtable_t time_ops = {.sleep_ms = startup_sleep};
static const h2_pal_time_api_t startup_clock = {.vtable = &time_ops};

int main(void) {
  char storage[4096];
  h2_gizclaw_e2e_ledger_t ledger;
  h2_esp_e2e_console_t console = {.uart = &uart, .timeout_ms = 5000u};
  assert(h2_gizclaw_e2e_ledger_init(&ledger, storage, sizeof(storage)) == 0);
  /* The App is still in the BLE wait: the host must already have its ready. */
  assert(h2_gizclaw_e2e_devkit_capture(&ledger, &console, ready, strlen(ready)) == 0);
  assert(writes == 1u && drains == 1u);
  assert(ledger.records == 1u && ledger.size == strlen(ready));
  assert(memcmp(ledger.data, ready, ledger.size) == 0);
  assert(strstr(delivered, "window=h2e2e-0123456789abcdef") != NULL);
  assert(h2_gizclaw_e2e_ledger_freeze(&ledger) == 0);
  /* Other evidence, including private fixture content, is captured only. */
  assert(h2_gizclaw_e2e_ledger_init(&ledger, storage, sizeof(storage)) == 0);
  const char secret[] = "H2_GIZCLAW_E2E stage=private secret=never-stream\n";
  assert(h2_gizclaw_e2e_devkit_capture(&ledger, &console, secret, strlen(secret)) == 0);
  assert(writes == 1u && strstr(delivered, "never-stream") == NULL);
  for (unsigned scenario = 0u; scenario < 3u; ++scenario) {
    writes = drains = 0u;
    write_error = scenario == 0u ? H2_PAL_ERR_TIMEOUT : 0;
    short_write = scenario == 1u;
    drain_error = scenario == 2u ? H2_PAL_ERR_IO : 0;
    console.error = 0;
    assert(h2_gizclaw_e2e_ledger_init(&ledger, storage, sizeof(storage)) == 0);
    assert(h2_gizclaw_e2e_devkit_capture(&ledger, &console, ready, strlen(ready)) != 0);
    assert(h2_gizclaw_e2e_ledger_freeze(&ledger) != 0);
    unsigned calls = writes;
    assert(h2_gizclaw_e2e_devkit_capture(&ledger, &console, ready, strlen(ready)) != 0);
    assert(writes == calls); /* A later replay cannot clear first output failure. */
  }
  write_error = drain_error = 0;
  short_write = false;
  const char *invalid[] = {
      "H2_GIZCLAW_E2E stage=ble-binding-ready window=h2e2e-0123456789abcdef revision=0 url_bytes=112 url_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n",
      "H2_GIZCLAW_E2E stage=ble-binding-ready window=h2e2e-0123456789abcdef revision=42 url_bytes=1025 url_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n",
      "H2_GIZCLAW_E2E stage=ble-binding-ready window=h2e2e-0123456789abcdef revision=18446744073709551616 url_bytes=112 url_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n",
      "H2_GIZCLAW_E2E stage=ble-binding-ready window=h2e2e-0123456789abcdef revision=42 url_bytes=112 url_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb secret=never-stream\n",
  };
  for (size_t i = 0u; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    assert(h2_gizclaw_e2e_ledger_init(&ledger, storage, sizeof(storage)) == 0);
    unsigned calls = writes;
    assert(h2_gizclaw_e2e_devkit_capture(&ledger, &console, invalid[i], strlen(invalid[i])) != 0);
    assert(writes == calls && h2_gizclaw_e2e_ledger_freeze(&ledger) != 0);
  }
  char execution[33] = {0};
  assert(h2_gizclaw_e2e_devkit_execution(&crypto, execution) == 0);
  assert(strcmp(execution, "0102030405060708090a0b0c0d0e0f10") == 0 && random_calls == 1u);
  assert(h2_gizclaw_e2e_devkit_execution(&crypto, execution) != 0 && random_calls == 1u);
  execution[0] = '\0'; zero_random = true;
  assert(h2_gizclaw_e2e_devkit_execution(&crypto, execution) != 0 && execution[0] == '\0');
  zero_random = false; random_error = H2_PAL_ERR_IO;
  assert(h2_gizclaw_e2e_devkit_execution(&crypto, execution) == H2_PAL_ERR_IO);
  assert(execution[0] == '\0');
  execution[0] = '\0'; random_error = 0;
  unsigned calls_before_wait = random_calls;
  sleep_error = H2_PAL_ERR_TIMEOUT;
  assert(h2_gizclaw_e2e_devkit_prepare_execution(&startup_clock, &crypto, execution) == H2_PAL_ERR_TIMEOUT);
  assert(random_calls == calls_before_wait && execution[0] == '\0');
  sleep_error = 0;
  assert(h2_gizclaw_e2e_devkit_prepare_execution(&startup_clock, &crypto, execution) == 0);
  assert(startup_waits == 2u && random_calls == calls_before_wait + 1u);
  assert(strcmp(execution, "0102030405060708090a0b0c0d0e0f10") == 0);
  puts("gizclaw immediate ready / first failure / boot nonce PASS");
  return 0;
}
