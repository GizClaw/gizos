# Drivers API

## CW2017 battery readings

`h2_cw2017_read_voltage()` and `h2_cw2017_sample_t.voltage_uv` return integer
microvolts. Decode VCELL at `0x02` as big-endian, mask to 14 bits, and compute
`floor(raw * 625 / 2)`. One count is 312.5 microvolts; the half microvolt is
rounded down. For example, `0x0001` returns 312, `0x0002` returns 625, and
`0x3fff` returns 5,119,687 microvolts. The upper two bits do not affect voltage.

`h2_cw2017_sample_t.soc_q8` preserves the big-endian SOC register at `0x04` in
exact units of 1/256 percentage point. The inclusive valid range is 0..25,600
(0..100 percent): `0x0001` is 1/256 percent, `0x3280` is 50.5 percent,
`0x63ff` is 99 + 255/256 percent, and `0x6400` is exactly 100 percent.
`0x6401` and larger values return `H2_PAL_ERR_WOULD_BLOCK` and clear both
sample fields. An independent voltage read remains available. Transport errors
propagate unchanged and clear required outputs; the two-register sample is
not an atomic hardware snapshot. Callers own battery-profile programming,
bus serialization and readiness policy.

<!--@include: ../.generated/api/drivers.md-->
