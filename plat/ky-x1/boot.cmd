# U-Boot script: boot Planar from the SD card (V4 step 6). The stock
# U-Boot's distro boot finds this as /boot.scr on the card's FAT partition
# and runs it, so the board boots to the Shell on power-on with nobody at
# the console; EndShell resets it.
#
# This U-Boot autoboots after 0 seconds and takes no Ctrl-C while it does,
# so a test run gets its prompt another way: with EFI/planar/hold on the
# card (Touch makes it, from the Shell; tools/serial-boot.py does that)
# this stops, and U-Boot goes on to its prompt. Delete it to boot again.
#
# The PMIC's watchdog has no driver yet, so it's stopped here; the SoC's
# the kernel takes over (plat/ky-x1/wdt.c).
if test -e ${devtype} ${devnum}:${distro_bootpart} EFI/planar/hold; then
    echo "Planar held: EFI/planar/hold is on the card"
else
    wdt dev PMIC_WDT
    wdt stop
    setenv bootargs 'shell reboot'
    load ${devtype} ${devnum}:${distro_bootpart} 0x200000 EFI/planar/planar.bin
    booti 0x200000 - ${fdtcontroladdr}
fi
