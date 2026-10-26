# Stability & volatility analysis — 2026-10-26

Paired same-toolkit comparison (CUDA 13.4, medians of 9, boot-verified states:
locked boot `StartEnable=0`, unlocked boot full tower to phase 13).

## Results (TFLOPS)

| workload | FP16 locked | FP16 unlocked | Δ | BF16 locked | BF16 unlocked | Δ |
|---|---|---|---|---|---|---|
| GEMM 2048³ | 24.39 | 27.32 | **+12.0%** | 26.13 | 28.93 | **+10.7%** |
| GEMM 4096³ | 28.02 | 31.38 | **+12.0%** | 29.40 | 30.87 | **+5.0%** |
| GEMM 8192³ | 35.64 | 36.71 | +3.0% | 36.36 | 36.67 | +0.9% |
| tall 16384×4096×4096 | 35.95 | 37.04 | +3.0% | 35.90 | 37.10 | +3.3% |

**Reading:** the compute-bound shapes show the unlock cleanly (+10–12%).
The large/power-saturated shapes compress toward zero delta — at 200 W the
board power cap binds before the issue-rate cap does. This is the low-power
row of the upstream results table behaving exactly as documented.

## Stability (unlocked session)

- Consecutive-run trace at 4096³ FP16: mean 33.75 TFLOPS, CV 5.7% at a steady
  1965 MHz — the unlocked card's variance is *lower* than the locked trace's
  pre-throttle CV (7.0%).
- Zero TDR events, zero WHEA corrections, zero display anomalies across the
  whole unlocked session (hours, including multiple benchmark suites).
- The unlock is selector-register state: digital, not analog. It does not
  degrade, drift, or partially apply. nvlddmkm binds normally on top of it.

## Volatility (by physics, by design)

- Power removal (shutdown / reboot / sleep) clears the selectors — confirmed
  by design and by the locked boot measuring stock rates after disarm+reboot.
- The armed filter re-establishes the state at every GPU start → effectively
  persistent across reboots, physically volatile underneath.
- Disarm → next boot is bit-identical to stock: no flash, no patched driver,
  no persistent writes of any kind (SECURITY.md).
- Known gap (inherited from the gen-1 analysis, unchanged): a sleep/resume
  cycle without a full reboot relies on the resume path re-running device
  start; not tested on this machine — if a post-resume benchmark ever reads
  locked rates on an armed boot, that is the mechanism, and a normal reboot
  restores it.

## Methodology notes (honesty ledger)

- cuBLAS 13.4 on sm_86 rejects several batched/skinny GEMM shapes
  (`GemmStridedBatchedEx`/`GemmEx` invalid-value, library-side, locked and
  unlocked alike) — batched and skinny duties were removed rather than
  present biased data.
- The locked trace's late-run clock droop (735/660 MHz) is thermal throttling
  after the sustained suite; suite medians are taken pre-throttle.
- Single machine, single session pair, air-cooled 46 °C start — treat as a
  careful measurement, not a survey.
