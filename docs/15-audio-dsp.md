# 15. Audio and DSP subsystem (`audio`, `audio.library`, `dsp.library`, `realtime.library`)

**Status: proposed (2026-10-20).** Builds on the zero-copy IPC rings ([§4](04-ipc.md)), coprocessor queue leases ([§5](05-coprocessors.md)), SIMD policy ([§7.4](07-language-toolchain.md#74-simd-policy)), driver model ([§11](11-driver-model.md)) and fault-tolerance rules ([§12](12-let-it-crash.md)).

AmigaOS gave programmers two things modern desktop audio stacks lost: **deterministic, microsecond-direct access to voices and DMA buffers** (`audio.device` and Paula) and **a retargetable, low-jitter voice and mixing abstraction** (AHI), coordinated by a shared system-wide musical clock (`realtime.library`).

Planar scales that architecture up for 64-bit heterogeneous SBCs with 8–12 CPU cores, wide SIMD (RVV 1.0, NEON, SVE2), multi-stream HDA/I²S/DisplayPort audio controllers and on-SoC audio DSPs:

- **Paula's 4 DMA channels in 2 MiB Chip RAM** become **`audio.library`**: an arbitrary number of sample-accurate virtual voices (`AudioVoice`) reading directly from shared `Area`s in the single address space without copying sample data.
- **Paula's precedence and channel-stealing protocol (`ADCMD_ALLOCATE` / `ADCMD_LOCK`)** becomes **priority-arbitrated streams and exclusive hardware DMA leases**, allowing a synth or game to drive a hardware output stream directly while still letting higher-precedence system alerts preempt or mix cleanly.
- **Fixed hardware RC/LED filters** become **`dsp.library`**: a zero-allocation, planar `F32` unit-generator and patch-graph engine running either on CPU SIMD (RVV/NEON) or on a hardware audio DSP through a §5.1 **queue lease**.
- **CIA-timer `realtime.library` Conductors** become a **lockless shared-memory Conductor page** driven by the hardware audio DMA frame clock and readable from any process with zero syscalls.

---

## 15.1 Architecture and layer split

```
 ┌─────────────────────────────────────────────────────────────────────────┐
 │ Applications   Trackers · Synths · Games · Media players · Shell bell   │
 ├─────────────────────────────────────────────────────────────────────────┤
 │ Libraries      audio.library    (voices, buses, streaming rings, AHI)   │
 │ (caller RX,    dsp.library      (SIMD filters, envelopes, FFT, graphs)  │
 │  SIMD-enabled) realtime.library (Conductors, sample clock, MIDI rings)  │
 ├─────────────────────────────────────────────────────────────────────────┤
 │ Servers        audio            (master RT mixer, precedence, leases)   │
 │ (own domain)   dsp-*            (optional hardware ADSP queue leases)   │
 ├─────────────────────────────────────────────────────────────────────────┤
 │ Drivers        hda (CIX ALC269) · i2s (RK3588/K1) · dp-audio · virtio   │
 └─────────────────────────────────────────────────────────────────────────┘
```

| Component | Kind | Role |
|---|---|---|
| `hda` / `i2s` / `virtio-snd` | Driver | Binds to `hwdb` audio controller nodes ([§11.6](11-driver-model.md#116-matching-and-binding)). Programs hardware DMA rings (HDA BDL, I²S cyclic DMA), codecs and period IRQs. |
| `audio` | Server | Owns the active endpoints, runs the real-time master mixing quantum on a dedicated high-priority task, enforces stream precedence, updates the shared `Conductor` clock page, and grants exclusive DMA leases. |
| `audio.library` | Shared library | Client-side API scaling up `audio.device` and AHI: voice tables, sample-accurate parameter ramps, double-buffered stream rings, and zero-copy sample `Area` management. |
| `dsp.library` | Shared library | Pure C23 + RVV 1.0 / NEON DSP primitives and DAG evaluator over planar 32-bit float (`F32`) buffers: biquad/SVF filters, resamplers, envelopes, dynamics, delay lines and partitioned convolution. |
| `realtime.library` | Shared library | Scales up Amiga `realtime.library` and `camd.library`: lockless sample-frame/tempo `Conductor` readers and zero-copy timestamped MIDI/control event rings. |

---

## 15.2 Scaling up the Amiga audio model

| Classic Amiga primitive | Limitation on 1985 hardware | Planar 64-bit scaled-up design |
|---|---|---|
| **Paula Chip RAM DMA** | 2 MiB address limit; 8-bit signed PCM only; max ~28.8 kHz in normal display modes | Any page-aligned `Area` in the 64-bit single address space (`ShareArea(..., AREAF_READ)`). Native `F32` internal pipeline; zero-copy hardware/SIMD decode from `S8`, `S16_LE`, `S24_LE`, `S32_LE` and `F32` at 48 / 96 / 192 kHz. |
| **4 fixed hardware channels** (`2 L` + `2 R`) | Hard 4-voice ceiling; fixed stereo hard-panning | Arbitrary voice banks (`AudioVoice`, 64–256+ voices per client or server bus) with continuous stereo pan, balance, or multichannel surround routing. |
| **`ADCMD_ALLOCATE` / `ADCMD_LOCK` precedence** | Stealing 1 of 4 physical registers muted the app's voice abruptly | **Precedence-arbitrated buses & hardware streams** (`-128` to `+127`). Exclusive hardware DMA leases can be preempted by higher-precedence clients with a clean `5 ms` cosine crossfade and `AUDMSG_STOLEN` / `AUDMSG_RESTORED` notifications. |
| **`CMD_WRITE` double-buffered chaining** | Kernel/device interrupt per buffer half | **Zero-trap SPSC audio rings** in shared `ConnArena`s plus `WaitAddr` on the 64-bit hardware quantum sequence number. No kernel trap while the producer stays ahead of the playhead (P4). |
| **`ADCMD_PERVOL` / `ADCMD_WAITCYCLE`** | Step changes in volume/period could click on zero-crossing misses | **Sample-accurate parameter queues**: every voice command carries a target `sample_frame` and an optional micro-ramp duration (`ramp_frames`) so gain and pitch changes never click. |
| **Paula 7 kHz LED low-pass filter** (`CIAAPRA` bit 1) | One global on/off analog switch | Per-voice and per-bus **`DSP_FilterSVF` / biquad chain**, including an exact `DSP_MODE_PAULA_LED` RC + 2-pole Butterworth emulation mode for classic tracker playback alongside modern parametric EQ. |
| **AHI `ahiam_PlayerFunc` tick hook** | Executed in interrupt context with strict stack limits | User-mode real-time task woken via `WaitAddr` on exact sample-frame deadlines, running safely inside the application's own protection domain. |
| **`realtime.library` Conductor** | 833 µs CIA timer resolution; drift between audio and tempo | **Hardware-locked `Conductor` page** driven directly by the DAC DMA frame counter (`sample_frame` + `CNTVCT_EL0` / `rdtime` correlation), giving sub-microsecond audio/video/MIDI lock. |

---

## 15.3 Data path and zero-copy rings

### Canonical buffer format

Inside `audio.library`, `dsp.library` and the `audio` server, the canonical working format is **non-interleaved (planar) 32-bit IEEE 754 floating-point (`F32`)**, aligned to 64-byte cache lines (`VLEN=256` on RV2, 128-bit NEON / SVE2 on Arm):

- Every channel of a quantum ($N = 64, 128 \text{ or } 256$ frames, default `128 frames = 2.67 ms` at `48 kHz`) is a contiguous `float[N]` array.
- Planar layout eliminates gather/scatter shuffles in RVV and NEON inner loops, lets mono voices plug straight into stereo/multichannel nodes, and gives `> 1500 dB` of internal dynamic range before the final hardware limiter and integer packer (`S32_LE` / `S24_LE` / `S16_LE`) at the DAC boundary.

### Three operating modes

Clients open a connection to `audio` through `audio.library` and select one of three data paths:

```
Mode A: Stream ring (DAWs, synths, media)
  App Area (planar F32 ring) ──ShareArea(RO)──▶ [ audio server SIMD mixer ] ──▶ DAC DMA Area

Mode B: Retained Voice Bank (Paula / AHI style — games, trackers, UI sounds)
  App Sample Areas (RO) ─────ShareArea(RO)────▶ [ audio server Voice Engine ] ─▶ DAC DMA Area
  App VoiceCmd ring ─────────(pitch/gain/trig)─┘

Mode C: Exclusive Hardware Lease (AUDF_EXCLUSIVE, precedence ≥ +50)
  App Area (hardware DMA ring) ═══════════════════════════════════════════════▶ DAC DMA Area
```

1. **Mode A — Zero-trap PCM stream ring**:
   - The client allocates a shared ring `Area` holding $K$ quantum slots (typically $K = 2\text{ or }4$) and shares it read-only with `audio`.
   - Each quantum, `audio` reads the next ready slot, mixes it into the master bus using SIMD, advances `read_seq` in the shared sync word, and calls `WakeAddr` only if the client set the `waiting` flag ([§4.3](04-ipc.md#43-fast-path-no-trap-while-the-receiver-is-awake)).
2. **Mode B — Retained Voice Bank (Scaled-Up Paula / AHI)**:
   - The client loads sample waveforms (one-shots, tracker instruments, UI chimes) into read-only `Area`s once and shares their handles with `audio`.
   - To trigger or modulate sounds, the client writes compact 32-byte `AudioVoiceCmd` structs (`VOICE_TRIG`, `VOICE_PITCH`, `VOICE_GAIN_RAMP`, `VOICE_PAN`, `VOICE_FILTER`, `VOICE_STOP`) into its connection ring, stamped with a target `sample_frame`.
   - The `audio` server steps the phase accumulators, interpolates samples directly out of the client's read-only sample `Area`s, and fires a reply event when a one-shot ends or a loop point is crossed (`ADCMD_WAITCYCLE`).
3. **Mode C — Exclusive Hardware DMA Lease (`AUDF_EXCLUSIVE`)**:
   - When an application requests `AUDF_EXCLUSIVE` with sufficient precedence (e.g. `+50`) and the hardware endpoint supports an independent stream (such as a dedicated HDA output stream tag or the sole I²S interface), `audio` grants the hardware DMA buffer `Area` and period doorbell/sync object directly to the client ([§5.1](05-coprocessors.md#51-the-abstraction)).
   - If a higher-precedence request arrives (`+100`, e.g. critical system alert) and no secondary hardware stream exists, `audio` sends `AUDMSG_STOLEN`, fades the exclusive stream over one quantum, plays the alert, and sends `AUDMSG_RESTORED` when done—matching `audio.device`'s `ADCMD_LOCK` protocol without dropping the client's state.

---

## 15.4 Fault tolerance: click-free "let it crash"

Applying [§12](12-let-it-crash.md) to real-time audio introduces a physical hazard: if a client process faults mid-waveform and the server immediately drops its contribution to `0.0f`, the resulting step discontinuity produces a loud **pop** on the speakers or headphones.

Planar's `audio` server enforces three crash-containment rules:

1. **Automatic de-click ramp on client death.**
   When a client crashes or closes its port (`CONNF_CLIENT_GONE`), the `audio` server captures the client's last output frame amplitude $(L_0, R_0)$ (or freezes its active `AudioVoice` phase accumulators) and applies a **240-frame (`5 ms` at 48 kHz) raised-cosine fade-out ramp** into the master mix before unsharing the client's `Area`s. A crashing synthesizer or game exits in complete silence without a single click.
2. **Underrun quarantine without blocking the mixer.**
   The `audio` server's real-time mixing task **never blocks** on a client (F3, [§12.5](12-let-it-crash.md#125-shared-memory-without-state-spill)). If a client misses its quantum deadline (`write_seq < expected_seq`), the mixer applies a `64-frame` micro-fade to zero for that client, increments the connection's `xrun_count`, and continues mixing all other clients on schedule.
3. **Master brickwall safety limiter.**
   Before converting the master `F32` mix bus to integer DAC samples, `audio` runs a zero-latency SIMD soft-clip / brickwall limiter at `-0.1 dBFS` and a DC-blocking high-pass filter (`5 Hz`) so a buggy client writing `1e6f`, `NaN` or `+Inf` into its buffer is sanitised (`isfinite` mask in SIMD) and clamped before reaching the amplifier.

---

## 15.5 `dsp.library`: SIMD and coprocessor DSP engine

`dsp.library` gives both applications and the `audio` server a deterministic, allocation-free library of digital signal processing primitives written in C23 with RVV 1.0 and NEON/SVE2 vector kernels ([§7.4](07-language-toolchain.md#74-simd-policy)).

### Core unit generators (UGens)

All UGens operate on `float * restrict` planar buffers of length $N$ (multiple of 16 frames), taking their state from caller-owned, cache-aligned structs so `dsp.library` allocates zero heap memory during audio processing:

| UGen family | Entry points | Description |
|---|---|---|
| **Oscillators & phase** | `DSP_OscPhasor`, `DSP_OscBLEP` | Band-limited polyBLEP saw, pulse/PWM, triangle and sine generators; 64-bit phase accumulators (`0.32` fractional phase). |
| **Sample interpolation** | `DSP_InterpLinear`, `DSP_InterpHermite4`, `DSP_ResampleSinc8` | Pitch-shifting voice readers: 4-point cubic Hermite for real-time tracker/synth voices; 8-tap polyphase windowed-sinc for studio resampling; optional non-interpolated zero-order hold + 7 kHz LED filter for authentic Paula playback. |
| **Filters (`SVF` & Biquad)** | `DSP_FilterSVF`, `DSP_FilterBiquad`, `DSP_FilterPaula` | Cytomic zero-delay-feedback trapezoidal State Variable Filter (stable under fast audio-rate cutoff modulation: LP, HP, BP, Notch, Peak, Bell, Shelf) and cascaded Direct Form II Transposed biquads. |
| **Envelopes & ramps** | `DSP_RampLinear`, `DSP_RampCosine`, `DSP_EnvADSR` | Click-free parameter smoothing and exponential/linear ADSR envelope generators. |
| **Dynamics** | `DSP_Compressor`, `DSP_Limiter`, `DSP_SanitizeF32` | RMS/peak feed-forward compressor, lookahead brickwall safety limiter, and SIMD `NaN`/`Inf`/denormal scrubber. |
| **Delay & Convolution** | `DSP_DelayLine`, `DSP_FFTReal`, `DSP_ConvolvePartitioned` | Fractional modulated delay lines (chorus, flanger, waveguide) and uniform/non-uniform partitioned FFT convolution over read-only impulse-response `Area`s. |

### Hardware DSP offload via queue leases

Two of Planar's target boards include dedicated low-power audio DSPs alongside the main CPUs:
- **CIX P1 (Sky1)**: `sky1_audss0` audio subsystem with a Cadence Tensilica HiFi4 DSP and reserved memory in the DSDT `RSVL` carve-outs ([§6.2](06-platforms.md#62-hazards-per-board), [v3-plan](v3-plan.md)).
- **Radxa Dragon Q8B (SC8280XP)**: Qualcomm LPASS / Hexagon ADSP reached over GLINK/SMEM ([§1.4](01-spec-review.md), [§5.3](05-coprocessors.md#53-by-board)).

Rather than inventing a second coprocessor API, hardware audio DSP offload uses Planar's standard **queue lease** ([§5.1](05-coprocessors.md#51-the-abstraction)):
- A `dsp-sky1` or `dsp-lpass` server boots the audio DSP firmware and grants queue leases.
- `dsp.library`'s `DspGraph` evaluator can target either the caller's CPU (`DSP_PROVIDER_CPU_SIMD`) or a hardware DSP queue lease (`DSP_PROVIDER_LEASE`) using the same graph descriptor and 64-bit sync sequence numbers.
- Until the hardware DSP driver is active on a board, `DSP_PROVIDER_CPU_SIMD` runs the identical graph on an A520 or X60 efficiency core (**VQ36**).

---

## 15.6 `realtime.library`: Conductor and event routing

Classic Amiga multimedia software relied on `realtime.library` (`Conductor` structures tracking musical time) and `camd.library` (MIDI routing) so multiple independent programs could lock to one tempo and exchange note events.

Planar unifies and modernises both in `realtime.library`:

### 1. Lockless shared `Conductor` page

The `audio` server publishes a global read-only `Area` (`sizeof(AudioConductor) == 64` bytes, one cache line) mapped into every client that opens `realtime.library`:

```c
typedef struct alignas(64) {
    uint64_t seq;              // Odd while updating, even when consistent (seqlock)
    uint64_t sample_frame;     // Monotonic DAC sample frames elapsed since boot
    uint64_t cycle_stamp;      // CNTVCT_EL0 / rdtime at the start of this quantum
    uint32_t sample_rate;      // e.g. 48000 or 96000 Hz
    uint32_t quantum_frames;   // e.g. 128 frames
    uint32_t bpm_q16;          // Tempo in 16.16 fixed-point Beats Per Minute (e.g. 120.0)
    uint32_t ppq_ticks;        // Musical ticks (960 PPQ) elapsed in current transport run
    uint32_t transport_state;  // RT_STOPPED, RT_PLAYING, RT_RECORDING
    uint32_t pad;
} AudioConductor;
```

- Any process reads the exact current hardware sample frame and musical beat position in user mode via a 6-instruction seqlock read loop (**zero kernel traps**).
- By correlating `sample_frame` with `cycle_stamp` (`CNTVCT_EL0` on Arm, `rdtime` on RISC-V, [§2.2](02-architecture.md#22-kernel-responsibilities)), a client can extrapolate the DAC playhead to the exact sample at any CPU cycle between interrupts, and `intuition` can align video vblank timestamps with audio frame numbers.

### 2. Sample-accurate MIDI and control rings (`RtEvent`)

Instead of 31.25 kbaud serial MIDI byte streams, `realtime.library` routes 16-byte `RtEvent` structs over standard Planar zero-copy SPSC connection rings (inspired by MIDI 2.0 UMP and CLAP note expressions):

```c
typedef struct {
    uint32_t frame_offset;     // Sample offset [0, quantum_frames) within current quantum
    uint8_t  kind;             // RT_NOTE_ON, RT_NOTE_OFF, RT_PARAM_MOD, RT_SYSEX_AREA
    uint8_t  channel;
    uint16_t note_or_param_id; // Note number or 16-bit parameter ID
    int32_t  target_voice_id;  // Optional per-note voice ID (-1 for channel-wide)
    float    value;            // Normalized velocity [0..1], pitch bend (semitones), or mod amount
} RtEvent;
```

Because `frame_offset` pins every note-on, note-off and parameter modulation to an exact sample within the quantum, connecting a sequencer process to a synthesizer process across protection domains introduces **zero timing jitter**.

---

## 15.7 Hardware support by board

| Board / Platform | Primary audio hardware | Driver / Server plan | Notes & quirks |
|---|---|---|---|
| **QEMU** (`qemu-virt-rv64`, `qemu-virt-a64`) | `virtio-snd` (or PCIe `intel-hda`) | `virtio-snd` driver + `audio` server | Used in CI (`make test-all`) with loopback/CRC capture to verify sample-accurate mixing, precedence preemption and crash de-clicking. |
| **CIX P1** (Orange Pi 6 Plus) | Sky1 HDA (`hdac0` at `0x70c0000`, IRQ 18) + Realtek ALC269 (`nid 20,21,24,25`); DisplayPort audio (`sky1_dptx0`); `sky1_audss0` I²S/DSP | `hda` driver (first hardware target), then `dp-audio` | **Quirk (measured in V3 dump):** `hdac0` reports an invalid RIRB size of `2` in `GCAP`; driver must override RIRB size to `256` entries (`docs/boards/cix-p1/freebsd-dmesg.txt:455`). DSDT `RSVL` carve-outs stay reserved for `sky1_audss0`. |
| **Orange Pi RV2** (SpacemiT K1) | K1 I²S controller + HDMI audio + USB Audio Class 2.0 | `i2s-k1` and `usb-audio` | DMA buffers require `NC/WC + Zicbom` cache maintenance if K1 audio DMA is non-coherent ([§5.2](05-coprocessors.md#52-cache-attributes)). |
| **Orange Pi 5 Plus** (RK3588) | `i2s0`–`i2s3` + ES8388 codec (I²C) + HDMI-QP audio | `i2s-rockchip` + `es8388` | Confined via per-block Rockchip IOMMU or SMMUv3 ([§3.7](03-memory.md)). |
| **Radxa Dragon Q8B** (SC8280XP) | Qualcomm LPASS / SoundWire / Hexagon ADSP + USB Audio | `usb-audio` first; native LPASS/ADSP later | Same PAS rule as CDSP ([§5.3](05-coprocessors.md#53-by-board)): never abruptly PAS-shut-down a running DSP. |

### Scheduler placement on big.LITTLE boards

On asymmetric boards (CIX P1's `8× A720 + 4× A520`, RK3588's `4× A76 + 4× A55`), the `audio` server's mixing task runs in the **real-time priority band** ([§2.7](02-architecture.md#27-smp)):
- Steady-state 64-voice SIMD mixing at `48 kHz` (`128-frame` quantum) takes `< 2%` of a single **A520** little core (`1.8 GHz`, SVE2/NEON), allowing the big A720 cores to sleep or run foreground compute while audio plays uninterrupted.
- Heavy client synthesis or convolution graphs can set task affinity to an A720 core (`SetAffinity`, [§2.2](02-architecture.md#22-kernel-responsibilities)).

---

## 15.8 Verification and conformance (`conformance/audio_*`)

Following P8 (*measure on the board*) and the mutation-tested conformance habit ([§8](08-roadmap.md)):

1. **`audio_voice_accuracy` (hosted & QEMU)**: Triggers 16 concurrent `AudioVoice` instances with known deterministic waveforms, pitch ramps and `VOICE_STOP` frame timestamps, captures the mixed output buffer, and checks bit-exact match (within `1e-6f` ULPs across RVV and NEON) against the reference C23 scalar evaluator.
2. **`audio_precedence_steal` (hosted & QEMU)**: Client *A* acquires an `AUDF_EXCLUSIVE` stream at precedence `+50`; Client *B* requests precedence `+100`. Verifies that Client *A* receives `AUDMSG_STOLEN`, the output transition contains no sample-to-sample step larger than the `5 ms` cosine ramp bound, and Client *A* receives `AUDMSG_RESTORED` when Client *B* closes.
3. **`audio_crash_declick` (hosted, QEMU & CIX P1)**: Client *A* outputs a full-scale `0 dBFS` DC/square wave and is killed mid-quantum with `KillProcess`. Asserts that `audio` detects `CONNF_CLIENT_GONE`, completes a `240-frame` smooth cosine ramp to `0.0f` with zero discontinuities, reclaims the shared `Area`, and never misses a hardware DMA deadline.
4. **Board latency probe (CIX P1 `hdac0`)**: Measures round-trip quantum wakeup jitter (`cycle_stamp` to mixer completion) across 10,000 quanta under CPU and IPC load (`vq3` soak test running concurrently); target `99.9th` percentile jitter `< 150 µs` at a `128-frame` (`2.67 ms`) quantum.

---

## 15.9 Open questions

- **VQ36 Hardware Audio DSP offload (`sky1_audss0` HiFi4, Q8B ADSP) vs. efficiency-core SIMD.**
  With 4× Cortex-A520 cores (with NEON and SVE2) on CIX P1 and 8× X60 cores (with 256-bit RVV 1.0) on RV2, one efficiency core can mix hundreds of voices and run convolution at minimal power without touching vendor DSP firmware. Should V5/V6 drive the on-SoC audio DSPs, or run `audio` + `dsp.library` on an efficiency core and keep the `dsp-*` queue lease optional?
  *Leaning:* run `audio` and `dsp.library` on CPU SIMD first (pinned to an A520 / cluster-1 core); keep the `DSP_PROVIDER_LEASE` hook in `dsp.library` so `sky1_audss0` can be bound later without changing client code.

- **VQ37 Native tracker / module format support in `audio.library` or `datatypes`.**
  Given the Amiga lineage and the retained `AudioVoice` engine (which natively supports sample `Area`s, period/pitch tables, loop points and the Paula LED filter mode), should a compact tracker player (`MOD` / `XM` or a native Planar `.pmod` format using `F32` samples) ship in user space as a standard library or `sound.datatype`?
  *Leaning:* ship a small, bounds-checked user-mode module player library (`mod.library`, `< 1,500` lines of C23) built directly on `audio.library`'s retained `AudioVoice` API as both a system utility and a real-world test of sample-accurate `Conductor` ticks.
