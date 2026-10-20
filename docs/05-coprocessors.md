# 5. Coprocessors: queue leases

## 5.1 The abstraction

**Proposed.** The paper's "firmware as hardware" becomes one user-facing object, the **queue lease**. The application side doesn't change between boards. Only the provider behind it changes.

| Component | Memory / attr | Who writes it |
|---|---|---|
| Command ring | Area shared with the device, attribute chosen per device (§5.2) | App |
| Doorbell | Either a hardware doorbell page mapped into the app (`AREAF_DEVICE`) or a word in shared memory that a server watches | App |
| Sync objects | 64-bit sequence number plus status, in shared memory | Device or server |
| Fault / reset notice | A signal bit and a message on the lease's port | Server |

```
app ──writes──▶ ring ──doorbell──▶ [ hardware queue  ]  ──▶ sync seqno ◀──polls/WaitAddr── app
                                   [ or server queue ]
```

Waiting on a sync object works two ways:
- Spin on the 64-bit sequence number.
- `WaitAddr` on it. The server or IRQ path calls `WakeAddr` when it changes.

**Providers:**

- **Hardware user queue:** the device's own firmware pulls from user rings. This is the paper's case. The kernel and the server are out of the data path.
- **Server queue:** a driver server owns the privileged ring. It takes app rings, validates them, and copies or chains them into the device. The app API is identical, but there is one extra hop.

The lease also does these things:
- Records which IOMMU or GPU-MMU context confines the device. It can also record "unconfined", per VQ9.
- Registers for reclamation. On process exit the provider drains or resets the queue before the memory is freed (step 4 in §2.3).

## 5.2 Cache attributes

**Decided rule: choose the attribute per device.** Don't apply "rings are non-cacheable" everywhere.

| Device | Coherent? | Ring and buffer attribute |
|---|---|---|
| Mali CSF (RK3588, CIX) | Depends on the SoC's ACE-Lite setup *(verify each)* | NC by default; WB + cache maintenance if coherent |
| Adreno 690 | I/O-coherent via SMMU *(verify)* | Same as Linux msm |
| Hexagon CDSP | **Coherent: it snoops CPU caches** | **WB.** NC breaks graphs (AbyssBSD npu.md). |
| CIX Zhouyi | Via SMMUv3 | Normal memory. Device-type mapping cost 10×. |
| K1 DPU / peripherals | Probably not coherent *(verify)* | NC/WC + Zicbom |

## 5.3 By board

**RK3588: Mali-G610 (Valhall CSF)**

The flagship case for the paper's model. Provider: hardware user queues.

`gpu-panthor` server:
- Loads `arm/mali/arch10.8/mali_csffw.bin`. The file name follows Linux convention *(verify)*.
- Boots the MCU.
- Schedules CSGs onto firmware slots when there are more groups than slots.
- Programs the GPU MMU address spaces with **identity** mappings of the app's grants.
- Maps the user doorbell page into the app.
- Handles faults and resets.

Reference: `OS/drm-panthor-kmod`.

**CIX P1: Immortalis-G720 MC10 (arch 12.8, CSF)**

Same server as the RK3588, using `arch12.8/mali_csffw.bin` (interface v3.13). The extra work is power-on, and that ordering belongs to the `power` server:
1. SCMI power via a TF-A SMC.
2. SCMI clocks via mailbox `CIXHA006`.
3. The `_ON` sequence.
4. Reset.
5. Q-channel.

Reading any GPU register before all five steps have run causes an SError.

**CIX P1: Zhouyi X2 NPU**

No firmware; jobs are register-driven. Provider: an `npu-aipu` server queue. Apps write job descriptors into their ring, and the server programs the job registers. SMMUv3 SID 0x1E confines it. Reference: `OS/aipu-kmod`.

**RK3588: RKNN (3 cores)**

Register-driven, with a Rockchip IOMMU per core. Provider: a server queue. Reference: Linux `drivers/accel/rocket`.

**Q8B: Adreno 690**

Not CSF. Provider: a server queue.

`gpu-msm` server:
- Owns the CP ringbuffer.
- Loads `a660_sqe.fw` and `a660_gmu.bin`.
- Has the zap shader authenticated through SCM.
- Switches per-process GPU page tables through CP commands.

Apps submit IBs through the queue lease. SMMU work must copy Linux's SMR values exactly, or the hypervisor resets the SoC. Reference: `OS/drm-msm-kmod`, AbyssBSD `gpu-display.md`.

**Q8B: Hexagon CDSP (HTP v68)**

Provider: an `npu-fastrpc` server over GLINK/SMEM. This is where the Qualcomm mailbox the paper mentions actually lives.

Constraints:
- Firmware is signed and loaded through PAS.
- **Never PAS-shut-down a running DSP.**
- Addresses sent to the DSP carry stream-ID bits above bit 32.
- QNN's userspace needs the QCS6490 identity trick.

Highest risk on any board (**VQ16**).

**K1: AI instructions in cluster 0 X60 cores**

Not a device *(verify)*. The "NPU" is extra architectural state on cluster-0 cores, so the scheduler handles it, not a lease:
- Tasks that use it get **affinity to cluster 0**.
- The extra state gets lazy save, like vector state.
- The 512 KiB TCM is allocated as an Area.

**K1: IMG BXE-2-32 GPU**

Firmware-scheduled through kernel CCBs. Provider: a server queue. Needs vendor driver sources; not in the mainline DT (**VQ17**).

**Display controllers (all boards)**

Not queue leases. The `display` server owns them (K1 DPU, VOP2, Linlon/komeda, Qualcomm MDSS), and apps get planes or screens.

Omega's virtual-chipset idea (a dedicated hart running the Copper and blitter) is out of scope for Planar v1. A hart could later act as a software provider for a "blitter" queue lease using the same API.

## 5.4 Reclamation and faults

- A device fault on a lease turns into a message to the lease holder. The provider resets only that queue or group if the hardware allows it (CSF groups can be terminated one at a time), and otherwise resets the unit.
- On process exit the provider stops the queue, waits for the device to stop touching the memory, and only then lets the kernel free the Areas. That wait is the step most likely to hang the Q8B, so the CDSP provider must use Qualcomm's safe teardown sequence.
