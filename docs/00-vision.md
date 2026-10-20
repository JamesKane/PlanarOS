# 0. Vision and principles

## 0.1 What Planar is

The vision paper describes a single-user OS for heterogeneous 64-bit SBCs with four properties:

1. **Universal pointers.** All processes share one address space, so a pointer means the same thing in every process and to every device.
2. **Protection is separate from translation.** Hardware keeps a crashing process from damaging the core. The protection exists to contain faults; the user still owns the machine.
3. **Exec-style IPC.** Messages travel by reference, ports are the rendezvous points, and payloads are never copied.
4. **Firmware as hardware.** Applications submit GPU and NPU work straight into firmware-visible rings and ring a doorbell. The kernel sets things up and then gets out of the way.

Planar keeps all four goals. [01-spec-review.md](01-spec-review.md) explains why several of the paper's *mechanisms* (PMP everywhere, editing access bits on every context switch, trap-free IPC through CPU mailboxes, CSF on every GPU) don't work on the target boards. It also gives the replacement for each.

## 0.2 Principles

**Proposed.** They are adapted from PlanOmega `architecture.md` §1–2, plus the paper's emphasis on determinism.

- **P1. One user, no adversary model.** Protection exists so that a buggy process can't take down the system. There are no logins, no ASLR and no stack canaries. W^X is on by default, with an explicit opt-out.
- **P2. A pointer is a pointer.** Each object has one address for its whole life. That address is the same in every protection domain. Wherever the hardware allows, it is also the address devices use (§3).
- **P3. The kernel provides mechanism and servers provide policy.** The kernel handles domains, tasks, scheduling, ports, signals, interrupt routing and leases. Everything else runs in user mode: drivers, filesystems, display, networking and coprocessor schedulers.
- **P4. Trap only when the kernel must act.** The paper wants trap-free IPC. Planar's version is that the fast path stays in user mode whenever the receiver is already awake. A trap is taken only to block, to wake a sleeper, or to change protection (§4).
- **P5. Determinism over throughput.** No demand paging, overcommit or swap. Allocations commit immediately. Latency limits are stated and measured.
- **P6. Every resource has an owner.** Everything is reclaimed when its owner exits, including messages still in flight, leases and coprocessor queues.
- **P7. Portability comes from the tree's structure, not from `#ifdef`s.** The rule is Omega's: portable code exists once, `arch/` and `plat/` are separate compile units, and a missing port function is a link error.
- **P8. Measure on the board.** Each claim about hardware costs or behaviour gets a probe that prints its result on the serial console. QEMU checks that the probe works, but the answer comes from the board.

## 0.3 Non-goals for v1

- POSIX compatibility as a design driver. A compatibility library may come later. It doesn't shape the kernel.
- Multiple users, networked security, sandboxing untrusted code.
- Running Linux GPU or NPU userspace (Mesa, QNN) unchanged. Planar borrows their *protocols*, not their ABI.
- Hypervisor or EL2 features.

## 0.4 Relationship to existing work

| Project | Relationship |
|---|---|
| **Omega / PlanOmega** | Planar's closest ancestor. It already ran a single address space with per-process domains, Exec ports and messages, lazy vector state and handle tables on the RV2, with measured costs: a 3,361-cycle `DoMsg` round trip and a 572-cycle context switch. Omega targets HellFire, which is big-endian with a mandatory MMU and a custom chipset. Planar targets real little-endian SoCs with commercial GPUs and NPUs. Planar copies whatever it can from omega and has no obligation to stay compatible with it (VQ1, decided). |
| **HellFire** | A 68k/ColdFire-lineage hobby CPU. The RV2 stands in for it in Omega. If Planar's protection back end can be swapped (§3.5), a HellFire or other MMU-less hobby core is one more `arch/` port. |
| **Earlier VectraOS corpus** (`../OS/designs`, `../OS/VectraOS*`) | A capability-secure, peer-to-peer distributed OS with one address space *per task*. It is a different system. This project began under the same working name and is now Planar (VQ2). Things worth taking: the Odin toolchain lessons (§7), the ADR habit, the conformance-by-serial-transcript method, and the AbyssBSD board notes (§6). Planar deliberately breaks from its memory and security model. |
| **AbyssBSD board work** (`../OS/AbyssBSD/docs/boards`) | First-hand notes on bringing up the Q8B and the Orange Pi 6 Plus, plus ports of Panthor, msm, komeda, aipu and FastRPC. This is the main hardware reference for the Arm boards. |
