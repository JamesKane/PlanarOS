# Orange Pi RV2: captured device tree

Taken on 2026-10-20 from the board, for hwdb's tests (docs/v4-plan.md step 2).

| File | What |
|---|---|
| `u-boot.dtb` | The tree the stock U-Boot (2022.10, in SPI flash) passes the kernel, 52,706 bytes: printed by the boot argument `dumpdtb` (`make PLATFORM=ky-x1 BOOTARGS=dumpdtb test`) and rebuilt from `build/ky-x1/serial-boot.log` with `tools/dtb-from-log.py`. |
| `hwdb.txt` | The report hwdb's graph must give from it (`make hwdb-test`). |

## Facts

- It's U-Boot's own control tree, not a Linux one: it has `binman` and `fit-images` nodes (which `dtc` calls errors), and its compatibles are the vendor's (`ky,x1-sdhci`, `ky,x1-ccu`), not Linux's `spacemit,k1-*`.
- The root compatible is just `"ky,x1", "riscv"`, so quirks keyed by it apply to any K1 board on this U-Boot. The model is `"ky x1 orangepi-rv2 board"`.
- Three SD/MMC controllers: `sdh@d4280000` (`mmc0`, the SD card, with a card-detect GPIO), `sdh@d4280800` (disabled) and `sdh@d4281000` (`mmc2`, eMMC).
- The PMIC (`ky,spm8821`) is on I2C 8 (`twsi8@d401d800`) at `0x41`.
- **Shared system controllers.** The clock controller, the reset controller and the power domains list the same register blocks: the MPMU at `0xd4050000` (also the watchdog's reset register, `0xd4051020`), the APMU at `0xd4282800` (shared again with the PCIe controller and its PHY), and others. Leases of whole pages can't give each its own.
