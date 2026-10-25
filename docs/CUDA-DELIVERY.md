# CUDA as delivery vehicle — assessment (post-baseline)

Question (from the machine's owner, after ordering the runtime port):
*"after that, consider if CUDA is not a more convenient delivery vehicle."*

## Verdict

**CUDA cannot deliver the unlock. It is, however, the correct judge.**

## Why CUDA cannot deliver

The unlock has two stages with distinct privilege requirements:

1. **PLM flip** (`FEAT_OVR_PLM`, `0x00823804` → `0xFFFFFFFF`). Only writable at
   L3/heavy-secure privilege — that mask is *the* protection. The only known
   door is the GSP-Booter ROP chain riding the firmware boot sequence, which
   requires raw BAR0 MMIO + falcon bootstrap + DMA: kernel-mode PCI access,
   before any driver owns the card.
2. **Selector writes** (`FEAT_OVR_SM_SPD[01]`). Host-writable MMIO once PLM is
   open — but "host" means a driver mapping BAR0. User-mode CUDA has no
   primitive for arbitrary device MMIO: the CUDA runtime sees SMs and memory
   through nvlddmkm's abstractions, never config space or BAR windows.

No CUDA API, NVML call, or cuBLAS/KDG path exposes any of: config-space R/W,
BAR0 mapping, FLR, falcon registers, or DMA buffer setup. A user-mode process
also cannot displace nvlddmkm from the device — PnP disable needs
administrator. Every step of the transaction is structurally out of CUDA's
reach.

## What CUDA is the right tool for

- **The judge**: `benchmark/gemm_bench.cu` (cuBLAS 4096×4096, FP16/BF16 tensor
  with FP32 accumulation, three-run medians — upstream's methodology).
  Locked baseline on record (`build/baseline-locked.txt`):
  `FP16 30.24 / BF16 30.41 TFLOPS` — exactly GA104's half-rate cap, proving
  the measurement honest and the lock real. Post-unlock expectation ≈ 2×
  (55–60 TFLOPS, power-limit dependent).
- **Workload verification**: any real workload (training step, prefill) can
  confirm the gain where it matters; decode-bound LLM inference will see none
  (memory-bandwidth-bound — upstream's own caveat).

## Actual delivery vehicles, ranked

| Vehicle | When | Cost |
|---|---|---|
| This runtime driver (`GuRtUnlock`) | on-demand unlock/relock mid-session | test-signing + one black window per run |
| UEFI arm (impacifist-style port, per-boot) | invisible, every boot, no runtime risk | one-time firmware-forensics port of the machine profile |
| Sleep/resume hook variant | re-arm on wake | same driver, PM-callback design |

The kernel driver (or its UEFI cousin) is the only door; CUDA is how you know
the door worked.
