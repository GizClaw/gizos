#include "flash_access_test_sdk.h"
#include "flash_access.h"
#include <assert.h>
#include <string.h>
#include <setjmp.h>

efuse_hw_t fake_efuse;
static unsigned active, calls, enabled, disabled, address, mode, flags, loads;
static uint8_t packet[34];
static jmp_buf worker_done;
static h2_pal_task_entry_t worker;
static unsigned worker_waits, request_kind, response_count;
static uint8_t* response;

int bk_efuse_driver_init(void) { return 0; }
uint32_t rtos_disable_int(void) {
  assert(!flags);
  flags = 1;
  return 7;
}
void rtos_enable_int(uint32_t f) {
  assert(f == 7 && flags);
  flags = 0;
}
uint64_t bk_aon_rtc_get_us(void) { return calls++ * 1000u; }
int efuse_ll_is_operate_finished(efuse_hw_t* p) {
  assert(p == &fake_efuse);
  return !active || mode != 2;
}
void efuse_ll_set_direction_read(efuse_hw_t* p) {
  assert(p == &fake_efuse && flags);
}
void efuse_ll_set_addr(efuse_hw_t* p, uint8_t a) {
  assert(p == &fake_efuse && a == 3);
  address = a;
}
void efuse_ll_enable(efuse_hw_t* p) {
  assert(p == &fake_efuse);
  active = 1;
  enabled++;
}
void efuse_ll_disable(efuse_hw_t* p) {
  assert(p == &fake_efuse);
  active = 0;
  disabled++;
}
int efuse_ll_is_rd_data_valid(efuse_hw_t* p) {
  assert(p == &fake_efuse);
  return mode != 3;
}
uint8_t efuse_ll_get_rd_data(efuse_hw_t* p) {
  assert(p == &fake_efuse && address == 3);
  return mode ? 32 : 0;
}
int bk_flash_read_bytes(uint32_t a, uint8_t* out, uint32_t n) {
  assert(a == 0x264000 && n == 34);
  memcpy(out, packet, n);
  return 0;
}
void h2_bk_test_cp_vector_copy(uintptr_t p, uint32_t* out) {
  assert(p == 0x2240000 && flags);
  loads++;
  memcpy(out, packet, 8);
}
int rtos_set_semaphore(beken_semaphore_t* e) {
  (void)e;
  return 0;
}
int rtos_get_semaphore(beken_semaphore_t* e, uint32_t t) {
  (void)e;
  assert(t == BEKEN_WAIT_FOREVER);
  if (worker_waits++) longjmp(worker_done, 1);
  return 0;
}
int rtos_init_semaphore(beken_semaphore_t* e, int count) {
  assert(count == 1);
  *e = 0;
  return 0;
}
int rtos_deinit_semaphore(beken_semaphore_t* e) {
  (void)e;
  return 0;
}
uint32_t mb_ipc_socket(uint8_t p, void* cb) {
  assert(p == 4 && cb);
  return 1;
}
uint32_t mb_ipc_server_get_connect_handle(uint32_t h, unsigned i) {
  assert(h == 1 && i < 3);
  return i + 10;
}
int mb_ipc_get_recv_event(uint32_t h, unsigned int* e) {
  if (h != 10 + request_kind) return -1;
  *e = MB_IPC_SEND_CMD;
  return 0;
}
int mb_ipc_get_recv_data_len(uint32_t h) {
  (void)h;
  return 16;
}
int mb_ipc_recv(uint32_t h, uint8_t* c, uint8_t* d, uint32_t n, uint32_t t) {
  (void)h;
  assert(n == 16 && t == 0);
  *c = 1;
  h2_bk_flash_status_request(d);
  if (request_kind == 1) d[0] ^= 1;
  if (request_kind == 2) *c = 2;
  return 16;
}
int mb_ipc_send(uint32_t h, uint8_t c, uint8_t* d, uint32_t n, uint32_t t) {
  assert(h == 10 && c == 1 && n == 16 && t == 500);
  assert(h2_bk_flash_status_valid(d, 1) && d[12] == H2_BK_FLASH_AES_ON);
  response = d;
  response_count++;
  return -1; /* Timed-out borrowed storage must remain valid. */
}
int mb_ipc_server_close(uint32_t h, uint32_t t) {
  (void)h;
  (void)t;
  return 0;
}
static int start_task(void* u, const h2_pal_task_options_t* o,
                      h2_pal_task_entry_t entry, void* ctx,
                      h2_pal_task_t** out) {
  (void)u;
  assert(!strcmp(o->name, "h2loader/flash-status") &&
         o->min_stack_size == 4096 && !ctx);
  worker = entry;
  *out = (h2_pal_task_t*)1;
  return 0;
}
const h2_pal_task_api_t* h2_bk_platform_task_api(void) {
  static const h2_pal_task_vtable_t vt = {.start = start_task};
  static const h2_pal_task_api_t api = {.vtable = &vt};
  return &api;
}
static void run_worker(unsigned kind) {
  request_kind = kind;
  worker_waits = 0;
  if (!setjmp(worker_done)) worker(NULL);
}
int main(void) {
  for (mode = 0; mode < 4; mode++) {
    calls = active = enabled = disabled = 0;
    assert(h2_bk_flash_test_read_control() ==
           (mode < 2 ? mode : H2_BK_FLASH_AES_UNKNOWN));
    assert(enabled == 1 && disabled == 1 && !flags && calls <= 7);
  }
  uint8_t query[16];
  h2_bk_flash_status_request(query);
  assert(h2_bk_flash_status_valid(query, 0));
  for (unsigned i = 0; i < 16; i++) {
    query[i] ^= 128;
    assert(!h2_bk_flash_status_valid(query, 0));
    query[i] ^= 128;
  }
  assert(h2_bk_flash_crc32(0, (const uint8_t*)"123456789", 9) == 0xcbf43926);
  uint32_t vector[2];
  memset(packet, 255, sizeof(packet));
  assert(h2_bk_flash_cp_vector(0x264000, vector) < 0 && !loads);
  memset(packet, 0, 32);
  uint16_t crc = 0xffff;
  for (unsigned i = 0; i < 32; i++) {
    crc ^= (uint16_t)packet[i] << 8;
    for (unsigned b = 0; b < 8; b++)
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0));
  }
  packet[32] = (uint8_t)(crc >> 8);
  packet[33] = (uint8_t)crc;
  assert(h2_bk_flash_cp_vector(0x264000, vector) == 0 && loads == 1);
  packet[0] ^= 1;
  assert(h2_bk_flash_cp_vector(0x264000, vector) < 0 && loads == 1);
  mode = 1;
  assert(h2_bk_flash_status_start() == 0 && worker);
  unsigned reads = enabled;
  run_worker(0);
  assert(response_count == 1 && response && response[12] == 1);
  run_worker(0);
  run_worker(1);
  run_worker(2);
  assert(response_count == 1 && enabled == reads && response[12] == 1);
  return 0;
}
