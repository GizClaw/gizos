// Preserve the SDK extraction contract without enabling any audio overlays.
OVERLAY : AT(0x200000) SUBALIGN(4)
{
    .overlay_aec { . = ALIGN(4); }
    .overlay_aac { . = ALIGN(4); }
} > ram0
