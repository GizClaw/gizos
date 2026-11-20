# Game Console R36S-V12, original panel. Hardkernel cfgload sources boot.scr on TF1.
setenv bootargs 'console=tty0 console=ttyS2,1500000n8 root=PARTUUID=36335030-02 rootfstype=squashfs rootwait ro'
if fatload mmc 1:1 0x09000000 Image && fatload mmc 1:1 0x01f00000 r36s.dtb; then
    booti 0x09000000 - 0x01f00000
fi
