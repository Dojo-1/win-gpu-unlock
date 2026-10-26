# win-gpu-unlock

Experimental **runtime** Windows port of [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock):
a WDM control-device driver that induces the Ampere GSP-Booter unlock transaction
(FP16/BF16/TF32 tensor issue-rate selectors) on a PnP-disabled GeForce card —
no reboot, no vBIOS flash, no driver replacement. GPL-2.0-only, derived from the
upstream Linux kernel module; `u610:NNN` anchors in the sources map every
translated block back to `kernel/gpu_unlock/unlock_610.c`.

**Status: EXECUTED ON HARDWARE (2026-10-26).** The full unlock transaction ran
at boot via the generation-2 start-IRP filter (`GuFltUnlock`): every phase to
selector-verification and post-FLR containment succeeded. Benchmark: FP16
35.61 / BF16 35.77 TFLOPS (+17.7% at the 200W board limit) vs the locked
30.24/30.41 baseline. See `docs/RESULTS.md` and `docs/TRACK2-CHARTER.md`.

## How it works (one paragraph)

The upstream exploit rides the GPU's boot sequence: a debug-signed Booter with a
stack overflow (ROP, single 4-byte write primitive) opens `FEAT_OVR_PLM`
(`0x00823804`), after which the tensor speed selectors `SS0/SS1` are plain
host-writable MMIO. Linux gets the pre-driver window for free; Windows does not.
This port recreates the window at runtime: the orchestrator PnP-disables the
card (driver detaches), then the driver's `induce` phase performs an FLR-class
reset that discards this power cycle's initialized state — the ROM boot replays
— and the full upstream transaction runs against the reopened window. The
cleanup FLR leaves the stock driver to initialize an unlocked card.

## Layout

| Path | Role |
|---|---|
| `driver/gu_rt.c/.h` | L4: control device, IOCTL, geodesic phase dispatcher, G2 gate |
| `driver/gu_transaction.c/.h` | L3: the port of `unlock_610.c` — falcon handshake, FWSEC, signature, selectors, dual-FLR containment |
| `driver/gu_env.h` | dual-mode shim (kernel / host) for the verbatim portable modules |
| `driver/portable/` | upstream `prep_610`, `vbios_610`, `layout_610`, `gfw_eval` — verbatim, one-line include redirect each |
| `driver/gu_fw_blobs.h` | generated: the five pinned firmware assets as arrays |
| `tools/make_fw_blobs.py` | blob generator (SHA-256 recorded per asset) |
| `tests/host_harness/` | mock-WDK harness: CI compiles the whole kernel branch off-WDK |
| `build.ps1` | WDK/clang-cl build + test-sign (gate G1) |
| `orchestrate.ps1` | L5: disable → IOCTL tower → enable, detached watchdog |
| `PORT-MANIFOLD.md` | design doc: strata, phase tower, +/− unwind table, gates |

## Build

```powershell
# assets (upstream pinned pipeline — hash-verified downloads):
cd ..\gpu-unlock\firmware   # clone of Zagitta/gpu-unlock
python fetch_artifacts.py && python generate_v67.py && python extract_boot_assets.py
# driver:
cd ..\win-gpu-unlock
.\build.ps1 -AssetDir ..\gpu-unlock\firmware
```

Requires LLVM 20.x (clang-cl/lld-link) and the WDK (headers/libs only).
Test-signing or Secure Boot off is mandatory for loading (gate G1).

## Use (gated)

```powershell
sc create GuRtUnlock type= kernel binPath= <abs>\build\GuRtUnlock.sys
sc start GuRtUnlock
.\orchestrate.ps1 -PhaseLimit 0                    # E1 probe — read-only, screen stays on
.\orchestrate.ps1 -PhaseLimit 3 -ConfirmBlackout   # through induce (first black window)
.\orchestrate.ps1 -PhaseLimit 12 -ConfirmBlackout  # full transaction (gate G2)
```

Phase tower: `0 probe · 1 assets · 2 claim · 3 induce · 4 gfw · 5 layout ·
6 dma · 7 fwsec · 8 signature(V67) · 9 boot · 10 signature(stock) ·
11 selector · 12 reset · 13 unwind`. Verify with CUDA GEMM before/after
(2× BF16 expected on GA104-class results upstream).

## Safety model

Fail-closed everywhere: unknown identity, open PLM, WPR2 already enabled, dead
config space, or any phase timeout refuses and unwinds; the watchdog re-enables
the device unconditionally; retention (firmware still running) pins the driver
and demands power-off, never frees live DMA buffers; V67 is never left
published. On any failure: full power off, `orchestrate.ps1` reports, retry.

## License

GPL-2.0-only (`LICENSE`). NVIDIA firmware assets (fetched by the upstream
pipeline) remain under NVIDIA's terms; do not commit them. Derived from
Zagitta/gpu-unlock — preserve its attribution and license headers.
