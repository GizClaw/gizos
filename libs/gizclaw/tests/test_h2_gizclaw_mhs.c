#include "h2_gizclaw_mhs_internal.h"
#include "payload/mhs.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct fixture {
  h2_gizclaw_mhs_value_t value;
  unsigned checks, writes;
  int check_error, write_error;
  bool clamp;
} fixture_t;
static int read_value(void *user, h2_gizclaw_mhs_value_t *out) {
  *out = ((fixture_t *)user)->value;
  return H2_PAL_OK;
}
static int check_value(void *user, const h2_gizclaw_mhs_value_t *value) {
  (void)value;
  fixture_t *f = user;
  ++f->checks;
  return f->check_error;
}
static int write_value(void *user, const h2_gizclaw_mhs_value_t *value,
                       h2_gizclaw_mhs_value_t *out) {
  fixture_t *f = user;
  ++f->writes;
  if (f->write_error)
    return f->write_error;
  f->value = *value;
  if (f->clamp)
    f->value.value.i = 42;
  *out = f->value;
  return H2_PAL_OK;
}
static unsigned live_allocations;
static int fail_after = -1;
static void *allocate(void *user, size_t size) {
  (void)user;
  if (fail_after == 0)
    return NULL;
  if (fail_after > 0)
    --fail_after;
  void *p = malloc(size);
  if (p)
    ++live_allocations;
  return p;
}
static void release(void *user, void *p) {
  (void)user;
  if (p) {
    assert(live_allocations);
    --live_allocations;
    free(p);
  }
}
static const h2_pal_mem_vtable_t memory_vtable = {.alloc = allocate,
                                                  .free = release};
static const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
static uint8_t encoded[16384];
static gizclaw_rpc_v1_ClientMhsV0WriteRequest write_request;
static gizclaw_rpc_v1_ClientMhsV0ReadRequest read_request;
static gizclaw_rpc_v1_ClientMhsV0WriteResponse reply;
static fixture_t fixtures[4];
static h2_gizclaw_mhs_state_t states[4];

static int run_bytes(bool write, const uint8_t *bytes, size_t length) {
  h2_gizclaw_rpc_provider_response_t response = {0};
  uint8_t *storage = NULL;
  int rc = h2_gizclaw_mhs_request_internal(
      states, 4, write, &memory, (h2_gizclaw_rpc_bytes_t){bytes, length},
      &response, &storage);
  if (rc == H2_PAL_OK) {
    memset(&reply, 0, sizeof(reply));
    pb_istream_t input =
        pb_istream_from_buffer(response.payload.data, response.payload.len);
    assert(pb_decode(&input, gizclaw_rpc_v1_ClientMhsV0WriteResponse_fields,
                     &reply));
  } else
    assert(!storage && !response.payload.data && !response.payload.len);
  h2_pal_mem_free(&memory, storage);
  assert(!live_allocations);
  return rc;
}
static size_t encode_request(bool write) {
  pb_ostream_t output = pb_ostream_from_buffer(encoded, sizeof(encoded));
  assert(pb_encode(&output,
                   write ? gizclaw_rpc_v1_ClientMhsV0WriteRequest_fields
                         : gizclaw_rpc_v1_ClientMhsV0ReadRequest_fields,
                   write ? (void *)&write_request : (void *)&read_request));
  return output.bytes_written;
}
static int run(bool write) {
  size_t n = encode_request(write);
  return run_bytes(write, encoded, n);
}
static void reset(void) {
  memset(fixtures, 0, sizeof(fixtures));
  memset(states, 0, sizeof(states));
  memset(&write_request, 0, sizeof(write_request));
  memset(&read_request, 0, sizeof(read_request));
  const char *keys[] = {"volume", "muted", "label", "position"};
  const h2_gizclaw_mhs_kind_t kinds[] = {
      H2_GIZCLAW_MHS_INT, H2_GIZCLAW_MHS_BOOL, H2_GIZCLAW_MHS_STRING,
      H2_GIZCLAW_MHS_DOUBLE};
  for (size_t i = 0; i < 4; ++i) {
    fixtures[i].value.kind = kinds[i];
    states[i] = (h2_gizclaw_mhs_state_t){"test.main", keys[i],     kinds[i],
                                         read_value,  check_value, write_value,
                                         &fixtures[i]};
    strcpy(write_request.states[i].device_id, states[i].device_id);
    strcpy(write_request.states[i].state, states[i].state);
    strcpy(read_request.states[i].device_id, states[i].device_id);
    strcpy(read_request.states[i].state, states[i].state);
    write_request.states[i].has_value = true;
  }
  write_request.states[0].value.which_value =
      gizclaw_rpc_v1_MhsValue_int_value_tag;
  write_request.states[1].value.which_value =
      gizclaw_rpc_v1_MhsValue_bool_value_tag;
  write_request.states[2].value.which_value =
      gizclaw_rpc_v1_MhsValue_string_value_tag;
  write_request.states[3].value.which_value =
      gizclaw_rpc_v1_MhsValue_double_value_tag;
  write_request.states_count = read_request.states_count = 4;
  fail_after = -1;
}
static void no_writes(void) {
  for (size_t i = 0; i < 4; ++i)
    assert(!fixtures[i].writes);
}

int main(void) {
  reset();
  assert(h2_gizclaw_mhs_validate_internal(states, 4) == H2_PAL_OK);
  assert(h2_gizclaw_mhs_validate_internal(NULL, 0) == H2_PAL_OK);
  assert(h2_gizclaw_mhs_validate_internal(NULL, 1) == H2_PAL_ERR_INVALID_ARG);
  const char *invalid[] = {"", "Bad", "a_foo", "a..b", "a-", "1a", NULL};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    reset();
    states[0].device_id = invalid[i];
    assert(h2_gizclaw_mhs_validate_internal(states, 4) ==
           H2_PAL_ERR_INVALID_ARG);
    reset();
    states[0].state = invalid[i];
    assert(h2_gizclaw_mhs_validate_internal(states, 4) ==
           H2_PAL_ERR_INVALID_ARG);
  }
  char long_key[66];
  memset(long_key, 'a', sizeof(long_key));
  long_key[65] = 0;
  reset();
  states[0].state = long_key;
  assert(h2_gizclaw_mhs_validate_internal(states, 4) == H2_PAL_ERR_INVALID_ARG);
  long_key[64] = 0;
  assert(h2_gizclaw_mhs_validate_internal(states, 4) == H2_PAL_OK);
  reset();
  states[1] = states[0];
  assert(h2_gizclaw_mhs_validate_internal(states, 4) == H2_PAL_ERR_INVALID_ARG);
  reset();
  states[0].read = NULL;
  assert(h2_gizclaw_mhs_validate_internal(states, 4) == H2_PAL_ERR_INVALID_ARG);
  reset();
  states[0].kind = (h2_gizclaw_mhs_kind_t)99;
  assert(h2_gizclaw_mhs_validate_internal(states, 4) == H2_PAL_ERR_INVALID_ARG);

  reset();
  write_request.states[0].value.value.int_value = 70;
  fixtures[0].clamp = true;
  assert(run(true) == H2_PAL_OK);
  assert(reply.states_count == 4 &&
         reply.states[0].value.value.int_value == 42);
  assert(reply.states[1].value.which_value ==
             gizclaw_rpc_v1_MhsValue_bool_value_tag &&
         !reply.states[1].value.value.bool_value);
  assert(reply.states[2].value.which_value ==
             gizclaw_rpc_v1_MhsValue_string_value_tag &&
         !reply.states[2].value.value.string_value[0]);
  assert(reply.states[3].value.which_value ==
         gizclaw_rpc_v1_MhsValue_double_value_tag);
  assert(run(false) == H2_PAL_OK &&
         reply.states[0].value.value.int_value == 42);
  for (size_t i = 0; i < 4; ++i) {
    assert(!strcmp(reply.states[i].device_id, states[i].device_id));
    assert(!strcmp(reply.states[i].state, states[i].state));
    assert(fixtures[i].checks == 1 && fixtures[i].writes == 1);
  }
  reset();
  write_request.states_count = read_request.states_count = 0;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG &&
         run(false) == H2_PAL_ERR_INVALID_ARG);
  reset();
  write_request.states[1] = write_request.states[0];
  read_request.states[1] = read_request.states[0];
  assert(run(true) == H2_PAL_ERR_INVALID_ARG &&
         run(false) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  strcpy(write_request.states[3].state, "unknown");
  strcpy(read_request.states[3].state, "unknown");
  assert(run(true) == H2_PAL_ERR_NOT_FOUND &&
         run(false) == H2_PAL_ERR_NOT_FOUND);
  no_writes();
  reset();
  write_request.states[1].value.which_value =
      gizclaw_rpc_v1_MhsValue_int_value_tag;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  write_request.states[3].has_value = false;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  write_request.states[3].value.which_value = 0;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  states[3].write = NULL;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  fixtures[3].check_error = H2_PAL_ERR_INVALID_STATE;
  assert(run(true) == H2_PAL_ERR_INVALID_STATE);
  no_writes();
  reset();
  fixtures[1].write_error = H2_PAL_ERR_IO;
  write_request.states[0].value.value.int_value = 13;
  assert(run(true) == H2_PAL_ERR_IO);
  assert(fixtures[0].writes == 1 && fixtures[0].value.value.i == 13 &&
         fixtures[1].writes == 1 && !fixtures[2].writes);
  reset();
  write_request.states[0].value.value.int_value = INT64_C(9007199254740992);
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  write_request.states[3].value.value.double_value = NAN;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  write_request.states[2].value.value.string_value[0] = (char)0xc0;
  write_request.states[2].value.value.string_value[1] = (char)0x80;
  assert(run(true) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  strcpy(write_request.states[2].value.value.string_value, "END");
  size_t length = encode_request(true);
  bool modified = false;
  for (size_t i = 0; i + 3 <= length; ++i)
    if (!memcmp(encoded + i, "END", 3)) {
      encoded[i + 2] = 0;
      modified = true;
      break;
    }
  assert(modified &&
         run_bytes(true, encoded, length) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  length = encode_request(true);
  encoded[0] = 0xff;
  assert(run_bytes(true, encoded, length) == H2_PAL_ERR_INVALID_ARG);
  no_writes();
  reset();
  fixtures[0].value.kind = H2_GIZCLAW_MHS_BOOL;
  assert(run(false) == H2_PAL_ERR_IO);
  reset();
  memset(fixtures[2].value.value.s, 'x', sizeof(fixtures[2].value.value.s));
  assert(run(false) == H2_PAL_ERR_IO);
  for (int i = 0; i < 2; ++i) {
    reset();
    fail_after = i;
    assert(run(false) == H2_PAL_ERR_NO_MEMORY);
  }
  return 0;
}
