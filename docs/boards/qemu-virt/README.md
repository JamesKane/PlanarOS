# QEMU virt: captured device trees and ACPI tables

Taken on 2026-10-20 with QEMU 11.1.2, for hwdb's and acpid's tests (docs/v4-plan.md steps 2 and 4).

| File | What |
|---|---|
| `rv64.dtb` | `qemu-system-riscv64 -machine virt -smp 4 -m 256M` with `virtio-rng-device`, as `mk/qemu-virt-rv64.mk` runs it, after OpenSBI's changes (its reserved memory, the PLIC's M-mode contexts unconnected). |
| `a64.dtb` | `qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a76 -smp 4 -m 256M` with `virtio-rng-device`, as `mk/qemu-virt-a64.mk` runs it with `-kernel`. |
| `rv64-hwdb.txt`, `a64-hwdb.txt` | The report hwdb's graph must give from each (`make hwdb-test`). |
| `a64-acpi/*.dat` | The ACPI tables edk2 gives the same AArch64 machine with `acpi=on` (`make PLATFORM=qemu-virt-a64 EFI=1 HW=acpi`). |
| `a64-acpid.txt` | What acpid must find in them, run on the host (`servers/acpid/host.c`): each device, and the count of what took a method. |
| `a64-acpi-hwdb.txt` | The report hwdb's graph must give from the tables with acpid's devices added. |

Both are the trees the kernel was given, printed by the boot argument `dumpdtb` and rebuilt with `tools/dtb-from-log.py`, which leaves out the padding after the strings (QEMU's AArch64 blob is padded to 1 MiB). To take them again:

    python3 -I tools/boot-check.py --expect 'V4.1 done' --log log -- \
        qemu-system-riscv64 ... -kernel build/qemu-virt-rv64/planar.bin -append dumpdtb
    tools/dtb-from-log.py log rv64.dtb

with the rest of the command line as `mk/qemu-virt-rv64.mk` has it (AArch64's `-append` starts with `qemu`).

Changing the machine's options (memory, CPUs, devices) changes the tree, and the reports with it: `make hwdb-test HWDB_UPDATE=1` rewrites them, and the diff is then the thing to read.

The ACPI tables are printed by the boot argument `dumpacpi` (`kernel/acpi.c`) and written back as files by `tools/acpi-from-log.py`:

    make PLATFORM=qemu-virt-a64 EFI=1 HW=acpi BOOTARGS=dumpacpi CHECK_FLAGS="--log log" test
    tools/acpi-from-log.py log a64-acpi
