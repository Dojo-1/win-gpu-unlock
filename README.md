# win-gpu-unlock

**The upstream Ampere unlock, transacted at every Windows boot — safely, serially, reversibly.**

An RTX 3060 Ti running the complete `gpu-unlock` transaction (FWSEC → GSP
Booter ROP → `FEAT_OVR_PLM` flip → tensor selectors) from inside the GPU's own
start IRP, held pending until the card is unlocked and only then handed to
nvlddmkm. The same window Linux gets by loading from initramfs — won on
Windows by owning the serialization instead of racing for it.

**Status: executed on hardware (2026-10-26).** Full tower to phase 13,
selectors verified twice (post-write and post-FLR), benchmark
**FP16 35.61 / BF16 35.77 TFLOPS (+17.7% at the 200 W board limit)** against
the locked 30.24/30.41 baseline. Every run mark, caveat, and the failure
archaeology that got here: [`docs/RESULTS.md`](docs/RESULTS.md).

---

## Read this first

This is a kernel driver that manipulates GPU firmware sequencing. It is
**locked to one card** — Zotac RTX 3060 Ti (`10DE:2489`, subsystem
`A632174B`) — and refuses everything else, fail-open. If that is not your
card, this repo is documentation, not software for you.

Before anything touches your machine, have the two documents that earned
their existence the hard way:

- **[`docs/TRACK2-CHARTER.md`](docs/TRACK2-CHARTER.md)** — the operating
  contract: single-completion IRP ownership, the durable crash counter,
  fail-open everywhere, staged gates. Written the day an earlier design
  bugchecked a machine into a reinstall.
- **[`docs/RECOVERY-KIT.md`](docs/RECOVERY-KIT.md)** — print it before you
  install. Under five minutes, from WinRE, to any recovery state. The
  project's total footprint is one service, one registry value, one file;
  there is no failure mode that requires reinstalling Windows.

## What it does

GeForce Ampere ships with FP16/BF16 tensor throughput capped at half the
professional rate. The upstream Linux module opens it by riding the card's
boot firmware: a debug-signed Booter with a stack overflow chains ROP to flip
one register — the Privilege Level Mask — after which the speed selectors are
plain host-writable MMIO. Linux wins that window by loading before the
driver. Windows never offers it at runtime (out-of-stack access hits D3cold;
the rebind race collides with nvlddmkm — both proven, see
[`PORT-MANIFOLD.md`](PORT-MANIFOLD.md) §5b). This project's generation-2
filter takes the only door left: a lower filter below nvlddmkm that holds
`IRP_MN_START_DEVICE` pending, transacts on the D0 card with no contender,
then completes the start. nvlddmkm binds onto an unlocked card and your
desktop comes up on schedule.

## The safety architecture

| Property | Mechanism |
|---|---|
| Cannot boot-loop | Crash counter is the first statement of `DriverEntry`, durably flushed; 3 bad loads → permanent pass-through until manual reset |
| Cannot run on the wrong card | VID/DID/subsystem verified in three independent places; refuse = normal boot |
| Cannot fire by accident | Class filter installs inert; `Parameters\Enable` gate; arming is an explicit admin act |
| Cannot fail closed | Wrong state, WPR2 up, assets missing, exception — every refusal completes the start untouched |
| Single-completion IRP | One `IoCompleteRequest` site by construction; the async-hold pattern that caused a `MULTIPLE_IRP_COMPLETE_REQUESTS` boot-loop is banned and documented |

## Quick start (supported card, accepted risk)

```powershell
# 0. Prereqs: VS2022 (C++), WDK 10.0.26100, Git, the upstream gpu-unlock clone
#    for the hash-pinned firmware pipeline. Windows test-signing enabled.

# 1. Generate the firmware assets (never committed; NVIDIA terms apply):
cd ..\gpu-unlock\firmware
python fetch_artifacts.py <dir> ; python generate_v67.py .. ; python extract_boot_assets.py .. <dir>
cd ..\win-gpu-unlock
python tools\make_fw_blobs.py <dir> driver\gu_fw_blobs.h

# 2. Build both drivers (mock pass first, then the real ones):
powershell -File ops\mock-pass.ps1          # must print: all checks passed
powershell -File build.ps1 -Out build2      # control device (diagnostics)
powershell -File build.ps1 -Out build2flt -Target guflt   # the filter

# 3. Inert install + verification reboots (docs/TRACK2-CHARTER.md Stage C):
powershell -File ops\stage-c.ps1            # UAC; then reboot TWICE
powershell -File ops\stage-c-verify.ps1     # marks must read Attached=1, BootCount=2, nothing run

# 4. Arm for boot (Stage D): recovery kit printed, work saved.
powershell -File ops\reset-counter.ps1      # UAC; Enable=1
#    Reboot. Early boot goes dark ~15-30 s longer while the tower runs.
#    Desktop returns on the result. Then:
.\benchmark\gemm_bench*.exe                 # the verdict
```

Everything is one-shot: the gate disarms, the counter re-arms itself for the
next boot, and `ops\stage-c-verify.ps1` reads the filter's breadcrumb marks
without elevation.

## Layout

| Path | What lives there |
|---|---|
| `driver/gu_transaction.c` | The faithful unlock-tower port, `u610:NNN` anchors to upstream |
| `driver/gu_flt.c` | Generation-2 start-IRP filter — the serialization hook |
| `driver/gu_rt.c` | Control-device driver (probing, diagnostics, the rebind-watch experiment) |
| `driver/portable/` | Upstream's parsers, compiled verbatim through a dual-mode shim |
| `ops/` | The staged lifecycle: install, verify, arm, recover |
| `tests/host_harness/` | Mock-WDK harness — the whole kernel branch compiles off-hardware, on two compilers |
| `benchmark/` | The CUDA judge (cuBLAS 4096², upstream methodology) |
| `PORT-MANIFOLD.md` | Design doc, evidence ledger, and the map of every dead end |
| `docs/` | Charter, recovery kit, results |

## Honest ledger

- The +17.7% is at a 200 W power limit — upstream's table puts the big
  numbers (+68–80%) on 350–450 W boards. Baseline and verdict ran on
  different CUDA versions (12.6 / 13.4); a same-toolkit locked baseline is
  one disarm-reboot-rerun away.
- The unlock is volatile by physics: selectors live until power drops, the
  filter re-establishes them every armed boot. That is the design, not a bug.
- This repository's history includes the failures that taught it: two
  bugchecks from a diagnostic breadcrumb, a boot-loop from the generation-1
  IRP pattern, a D3cold wall, a TDR collision. Each root-caused in
  `PORT-MANIFOLD.md` — kept visible on purpose, because they are the
  strongest documentation this project owns.

## License & attribution

GPL-2.0-only, ported from [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock)
with `u610:NNN` anchors. NVIDIA firmware assets are generated locally from
NVIDIA's own pinned packages and remain under NVIDIA's terms — never commit
them (`SECURITY.md`).
