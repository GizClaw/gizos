#include "h2_cw2017.h"

h2_pal_result_t h2_cw2017_read_version(const h2_cw2017_transport_t* transport,
                                       uint8_t* out_version) {
  if (out_version == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_version = 0;
  if (transport == NULL || transport->read_regs == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t value = 0;
  h2_pal_result_t rc = transport->read_regs(transport->user, 0x00, &value, 1);
  if (rc == H2_PAL_OK) *out_version = value;
  return rc;
}

h2_pal_result_t h2_cw2017_read_voltage(const h2_cw2017_transport_t* transport,
                                       uint32_t* out_voltage_uv) {
  if (out_voltage_uv == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_voltage_uv = 0;
  if (transport == NULL || transport->read_regs == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint8_t voltage[2] = {0};
  h2_pal_result_t rc = transport->read_regs(transport->user, 0x02, voltage, 2);
  if (rc != H2_PAL_OK) return rc;
  uint32_t raw = (((uint32_t)voltage[0] << 8) | voltage[1]) & 0x3fffu;
  *out_voltage_uv = raw * 625u / 2u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_cw2017_read_sample(const h2_cw2017_transport_t* transport,
                                      h2_cw2017_sample_t* out_sample) {
  if (out_sample == NULL) return H2_PAL_ERR_INVALID_ARG;
  *out_sample = (h2_cw2017_sample_t){0};
  if (transport == NULL || transport->read_regs == NULL)
    return H2_PAL_ERR_INVALID_ARG;
  uint32_t voltage_uv = 0;
  uint8_t soc[2] = {0};
  h2_pal_result_t rc = h2_cw2017_read_voltage(transport, &voltage_uv);
  if (rc != H2_PAL_OK) return rc;
  rc = transport->read_regs(transport->user, 0x04, soc, 2);
  if (rc != H2_PAL_OK) return rc;
  uint16_t soc_q8 = (uint16_t)(((uint16_t)soc[0] << 8) | soc[1]);
  if (soc_q8 > 25600u) return H2_PAL_ERR_WOULD_BLOCK;
  out_sample->voltage_uv = voltage_uv;
  out_sample->soc_q8 = soc_q8;
  return H2_PAL_OK;
}
