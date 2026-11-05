#include "h2_cw2017.h"
#include <assert.h>

typedef struct fixture {
  unsigned calls, fail_at;
  uint8_t soc_hi, soc_lo;
  uint8_t voltage_hi, voltage_lo;
} fixture_t;
static h2_pal_result_t read_regs(void* user, uint8_t reg, uint8_t* out,
                                 size_t len) {
  fixture_t* f = user;
  if (++f->calls == f->fail_at) return H2_PAL_ERR_TIMEOUT;
  if (reg == 0) {
    assert(len == 1);
    out[0] = 0x17;
  } else if (reg == 2) {
    assert(len == 2);
    out[0] = f->voltage_hi;
    out[1] = f->voltage_lo;
  } else {
    assert(reg == 4 && len == 2);
    out[0] = f->soc_hi;
    out[1] = f->soc_lo;
  }
  return H2_PAL_OK;
}
static void test_voltage_units(void) {
  const struct {
    uint16_t reg;
    uint32_t microvolts;
  } cases[] = {{0x0000, 0},       {0x0001, 312},     {0x0002, 625},
               {0x2f01, 3760312}, {0x3fff, 5119687}, {0xffff, 5119687}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    fixture_t f = {.voltage_hi = (uint8_t)(cases[i].reg >> 8),
                   .voltage_lo = (uint8_t)cases[i].reg};
    h2_cw2017_transport_t transport = {.user = &f, .read_regs = read_regs};
    uint32_t voltage = 99;
    assert(h2_cw2017_read_voltage(&transport, &voltage) == H2_PAL_OK);
    assert(voltage == cases[i].microvolts && f.calls == 1u);
  }
}

static void test_soc_precision_and_boundaries(void) {
  const uint16_t cases[] = {0x0000, 0x0001, 0x3280, 0x63ff,
                            0x6400, 0x6401, 0xffff};
  fixture_t f = {.voltage_hi = 0xef, .voltage_lo = 0x01};
  h2_cw2017_transport_t transport = {.user = &f, .read_regs = read_regs};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    f.soc_hi = (uint8_t)(cases[i] >> 8);
    f.soc_lo = (uint8_t)cases[i];
    h2_cw2017_sample_t sample = {.voltage_uv = 1, .soc_q8 = 1};
    const int result = h2_cw2017_read_sample(&transport, &sample);
    if (cases[i] <= 25600u) {
      assert(result == H2_PAL_OK && sample.soc_q8 == cases[i]);
      assert(sample.voltage_uv == 3760312u);
    } else {
      assert(result == H2_PAL_ERR_WOULD_BLOCK);
      assert(sample.voltage_uv == 0u && sample.soc_q8 == 0u);
      uint32_t voltage = 0;
      assert(h2_cw2017_read_voltage(&transport, &voltage) == H2_PAL_OK);
      assert(voltage == 3760312u);
    }
  }
}

int main(void) {
  test_voltage_units();
  test_soc_precision_and_boundaries();
  fixture_t f = {
      .soc_hi = 50, .soc_lo = 128, .voltage_hi = 0xef, .voltage_lo = 0x01};
  h2_cw2017_transport_t transport = {.user = &f, .read_regs = read_regs};
  uint8_t version = 99;
  assert(h2_cw2017_read_version(&transport, &version) == H2_PAL_OK &&
         version == 0x17);
  uint32_t uv = 0;
  assert(h2_cw2017_read_voltage(&transport, &uv) == H2_PAL_OK && uv == 3760312);
  assert(h2_cw2017_read_voltage(&transport, NULL) == H2_PAL_ERR_INVALID_ARG);
  uv = 99;
  assert(h2_cw2017_read_voltage(NULL, &uv) == H2_PAL_ERR_INVALID_ARG &&
         uv == 0);
  h2_cw2017_transport_t missing = {0};
  uv = 99;
  assert(h2_cw2017_read_voltage(&missing, &uv) == H2_PAL_ERR_INVALID_ARG &&
         uv == 0);
  f.calls = 0;
  f.fail_at = 1;
  uv = 99;
  assert(h2_cw2017_read_voltage(&transport, &uv) == H2_PAL_ERR_TIMEOUT &&
         uv == 0);
  f.fail_at = 0;
  h2_cw2017_sample_t sample;
  assert(h2_cw2017_read_sample(&transport, &sample) == H2_PAL_OK);
  assert(sample.soc_q8 == 12928 && sample.voltage_uv == 3760312);
  for (unsigned i = 1; i <= 2; ++i) {
    f.calls = 0;
    f.fail_at = i;
    sample.voltage_uv = 1;
    assert(h2_cw2017_read_sample(&transport, &sample) == H2_PAL_ERR_TIMEOUT);
    assert(sample.voltage_uv == 0 && sample.soc_q8 == 0);
  }
  f.fail_at = 0;
  for (unsigned soc = 0; soc < 256; ++soc) {
    f.soc_hi = (uint8_t)soc;
    f.soc_lo = 0;
    assert(h2_cw2017_read_sample(&transport, &sample) ==
           (soc <= 100 ? H2_PAL_OK : H2_PAL_ERR_WOULD_BLOCK));
  }
  f.soc_hi = 100;
  f.soc_lo = 1;
  assert(h2_cw2017_read_sample(&transport, &sample) == H2_PAL_ERR_WOULD_BLOCK);
  assert(h2_cw2017_read_sample(NULL, &sample) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_cw2017_read_sample(&transport, NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_cw2017_read_version(NULL, &version) == H2_PAL_ERR_INVALID_ARG &&
         version == 0);
  f.calls = 0;
  f.fail_at = 1;
  version = 99;
  assert(h2_cw2017_read_version(&transport, &version) == H2_PAL_ERR_TIMEOUT &&
         version == 0);
  return 0;
}
