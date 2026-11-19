"""SDK partition views of the Zero BK 2.0 fixed-XIP flash map."""

load("@bazel_skylib//rules:expand_template.bzl", "expand_template")

def zero_bk_partition_tables():
    """Emit the SDK's primary CP/AP and s_app rows for each executable slot."""
    boot_kib = 68
    cp_kib = 1156
    p1_ap_kib = 1292
    p2_ap_kib = 3876
    p1_offset = boot_kib * 1024
    p1_size = cp_kib + p1_ap_kib
    p2_offset = p1_offset + p1_size * 1024
    p2_size = cp_kib + p2_ap_kib
    for name, output, own_offset, ap_kib, other_offset, other_kib in [
        ("p1_partitions", "p1.csv", p1_offset, p1_ap_kib, p2_offset, p2_size),
        ("p2_partitions", "partitions/bk7258/auto_partitions.csv", p2_offset, p2_ap_kib, p1_offset, p1_size),
    ]:
        expand_template(
            name = name,
            out = output,
            template = "partitions/bk7258/fixed_xip.csv.in",
            substitutions = {
                "@BOOT_KIB@": str(boot_kib),
                "@CP_KIB@": str(cp_kib),
                "@AP_KIB@": str(ap_kib),
                "@OWN_OFFSET@": "0x%x" % own_offset,
                "@OTHER_OFFSET@": "0x%x" % other_offset,
                "@OTHER_KIB@": str(other_kib),
            },
        )
