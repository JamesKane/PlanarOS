# 4. IPC: Exec, with protection

## 4.1 Primitives

| Primitive | Status | Notes |
|---|---|---|
| **Signals** | Decided (Omega) | 64 bits per task, 16 of them reserved for the system. `Wait(mask)` is how every task blocks. |
| **Ports** | Proposed | A port is a named rendezvous kept in the kernel's name table, and the server holds a handle to it. **A port has no ring.** Clients reach it through connections. |
| **Connections** | Proposed | Made by `OpenPort(name)`. The kernel allocates an **arena** Area and maps it RW into exactly the client and the server. It puts a **request ring** and a **reply ring** at the start of the arena, and assigns the server a signal bit for the connection. It also registers death notices in both directions. |
| **Messages** | Proposed | Live inside an arena or inside an Area shared with the peer. They are passed by pointer and replied to in place. |
| **Futex** | Proposed | `WaitAddr`/`WakeAddr` on a word in shared memory. Higher-level locks are built on it. |

## 4.2 Message header

```c
struct Message {
    struct MinNode mn_Node;     /* free for the owner's use; queue links are NOT here */
    Handle   mn_ReplyPort;      /* handle, not an address: replies can't be forged */
    uint32_t mn_Length;         /* total length including the header */
    int32_t  mn_Error;          /* set by the replier; ERR_PORT_DIED on peer death */
    uint32_t mn_Flags;
    void    *mn_Payload;        /* universal pointer; must lie in memory both sides map */
    uint64_t mn_PayloadLen;
};
```

How this differs from the paper's struct:
- The reply port is a handle, not a physical address.
- There is an error field.
- The payload pointer must lie inside an Area that both sides can map. `IOF_LEND` (from Omega) maps a buffer Area into the server for the duration of a request.

## 4.3 Fast path: no trap while the receiver is awake

Each ring is single-producer, single-consumer, inside the arena:

```
ring { u32 head; u32 tail; u32 waiting; u32 pad; u64 slot[N]; }   // slot = message pointer
```

**Send** (client side):
1. Write the message into the arena.
2. Store the message pointer into `slot[head % N]`, then a release-store of `head + 1`.
3. If `waiting` is set: `Signal(conn)`. This is the **only trap**. The kernel wakes the server and sends an IPI if the server is on another core.

**Receive** (server side):
1. Drain the ring.
2. When it is empty, set `waiting`, check the ring again, then `Wait(mask)`. This is the usual check after setting the flag to avoid a lost wakeup.

**Reply** is the same in the other direction.

If the server is busy, a stream of requests costs **zero traps**. Only the 0→1 edge of "work available" while the receiver is sleeping costs one.

Rules:
- **The server copies fields it validates.** The client can still change the arena after sending (Omega rule). Servers copy the header and arguments out before checking them, the classic time-of-check/time-of-use defence.
- **Ownership is by convention, not enforced (Omega open question 2, kept).** After `PutMsg` the sender must not touch the message until the reply arrives. Breaking that rule can only corrupt the sender's own connection.
- **Full ring:** the sender either blocks on the reply ring's progress or gets `ERR_FULL` (**VQ10**).

## 4.4 Synchronous path: `DoMsg`

`DoMsg` is send, then wait for this message's reply.
- If the server is blocked on the connection, the kernel hands the CPU straight to it (handoff scheduling, as in L4 and QNX) with priority inheritance.
- Omega measured a 3,361-cycle round trip on the X60. Planar's target is to stay at or below that on the RV2, and to measure the same thing on each Arm board.

## 4.5 Cross-core notification

- **RISC-V:** IPIs use SBI `sbi_send_ipi`, which traps to OpenSBI. If the board turns out to have ACLINT SSWI, S-mode can write `SETSSIP` directly *(verify on the K1)*. That would be a `plat_` choice found by probe at run time.
- **AArch64:** IPIs are GICv3 SGIs written to `ICC_SGI1R_EL1` from EL1.
  - RK3588 puts the CPU index in **MPIDR Aff1**, so SGI targeting has to use the full affinity value.
- Either way, user mode never rings another CPU directly. P4 makes that unnecessary in the common case.

## 4.6 Interrupts to user mode

**Decided (Omega).** By default the kernel masks the source, signals the driver task, and unmasks on `AckIrq`. That is a PLIC claim/complete or a GIC EOI. A user-mode fast handler running inside the driver's domain is optional and comes later.

## 4.7 Public surface

- `exec.library` wraps all of this: `PutMsg`, `GetMsg`, `ReplyMsg`, `WaitPort` and `DoIO`/`SendIO` with `IORequest`. Code written to RKM habits reads naturally.
- The rings and the `waiting` protocol are an implementation detail. They are versioned in the arena header.
