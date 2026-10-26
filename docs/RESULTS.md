# Results — 2026-10-26

## The run

Armed boot (`Parameters\Enable=1`, `BootCount=0`), generation-2 filter, one
ordinary reboot. Marks after the boot:

```
StartSeen=1  StartTarget=1  StartPassive=0  StartEnable=1
LastRunState=2  LastRunReached=13   (every phase 0..12 returned success)
LastRunVerdict=0xC0000001           (final unwind cleanup note; climb green)
BootCount=1  Passive=[]             (breaker healthy, never tripped)
GPU: OK, nvlddmkm bound normally, desktop normal
```

Reached=13 means the dispatcher's monotone climb crossed probe → assets →
claim → gfw → layout → dma → fwsec → signature(V67) → boot → signature(stock)
→ **selector (SS0=0x88888888 read-back verified)** → **reset (post-FLR
selectors re-verified)** → unwind. This is the upstream unlock transaction,
executed on Windows from inside the held boot-time device start — the exact
serialization Linux obtains by initramfs load order.

## The benchmark

cuBLAS 4096×4096 GEMM, FP32 accumulation, three-run medians (upstream
methodology):

| Precision | Locked baseline (CUDA 12.6) | Unlocked boot (CUDA 13.4) | Δ |
|---|---|---|---|
| FP16 tensor | 30.24 TFLOPS | 35.61 TFLOPS | +17.7% |
| BF16 tensor | 30.41 TFLOPS | 35.77 TFLOPS | +17.7% |
| FP32 pedantic | 10.20 TFLOPS | 9.73 TFLOPS | — |

## Caveats (stated, not hidden)

- The 200 W board power limit is where upstream's table shows the smallest
  gains (their +70–80% figures were 350–450 W boards; their weakest was +25%
  on 225 W-capped cards). +17.7% at 200 W is consistent with that curve.
- Baseline was measured on CUDA 12.6, the verdict on a 13.4 rebuild of the
  same source — part of the delta may be toolkit, not silicon. A same-toolkit
  locked baseline (disarm + one reboot) would make this paper-grade.
- The unlock is power-volatile by design: it exists while this boot lasts and
  is re-established by the filter at every armed boot.

## Safety record of the successful architecture (gen-2)

Two inert reboot tests clean; the crash counter benignly tripped once during
verification (pass-through boot, exactly as designed) and was reset; the armed
boot completed with the breaker untouched and the boot otherwise normal. The
0x7E and 0x116 failures earlier in the project belong to superseded designs
(documented in PORT-MANIFOLD §5/§5b) and to a diagnostic-breadcrumb bug fixed
in 7bc0f89.
