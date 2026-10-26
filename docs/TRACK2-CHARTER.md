# Track 2 charter — the filter route under full safeguards

Written 2026-10-26, the day a generation-1 filter defect bugchecked the
primary machine into an OS reinstall. This document is the operative contract
for every further step on this route. If any rule here conflicts with
convenience, the rule wins or the route stops.

## What happened (the permanent record)

Generation 1 held the GPU start IRP with a completion routine + worker-thread
pattern. IRP completion ownership was ambiguous; a second completion path
fired; `MULTIPLE_IRP_COMPLETE_REQUESTS` bugchecked the display stack — and
because the filter was class-level and boot-persistent, the fault repeated at
every boot. The machine required a clean Windows 11 reinstall. Contributing
process failures: safety properties asserted without being engineered
("worst case is a locked card booting normally" was false); risk escalated
after each of three prior bugchecks instead of stopping; the most dangerous
change of the project shipped as a mid-run fix with no inert reboot test and
no boot escape hatch.

## Governing principles

1. **Single-completion by construction.** Exactly one code path may call
   `IoCompleteRequest` for the start IRP, reachable on every IRP fate, with no
   thread handoffs. Completion routines only signal events and return
   `STATUS_MORE_PROCESSING_REQUIRED` unconditionally. Any design needing a
   second thread to argue its safety is rejected at review.
2. **Boot loops are structurally impossible.** DriverEntry's first statement
   durably (ZwFlushKey) increments a crash counter; >= 3 → permanent
   pass-through until manual reset. Worst case ever: three crashes, then
   normal boots. Success resets the counter.
3. **Everything fails open.** Wrong hardware, gate off, WPR2 already up,
   assets unreadable, deadline exhaustion, exception inside the transaction
   (guarded *with* full unwind in the same frame) — every refusal ends in a
   normal Windows boot.
4. **The machine is not a lab.** Adversarial review before any build-to-
   deploy; CI compiles both drivers; any crash on the primary machine = full
   stop + uninstall, root-causing happens offline afterward.

## Verified safeguards in generation 2

- Synchronous-wait start hold (no worker, no `IoMarkIrpPending`, indefinite
  event wait by documented design — a timeout that completes early would
  re-create the double-completion edge).
- Crash counter with `ZwFlushKey` durability (adversarial-review finding
  F3.1: lazy hive flush would lose the increment exactly in the fast-crash
  case the counter exists for).
- SEH guard *inside* the transaction frame with real unwind (finding F6.1:
  the old placement leaked 16 MiB BAR0 mapping + ~90 MB DMA and handed
  nvlddmkm a torn GPU with a SUCCESS start).
- Explicit zeroed DMA minimum address (finding F5.1: `MmGetPhysicalAddress`
  on NULL is undocumented in exactly the computation that matters).
- Hardware guard (only VEN_10DE&DEV_2489 acts; the class filter attaches
  pass-through to every other display device).
- Registry gate `Enable` (0 = pure pass-through; arming is a deliberate act).

## Known acceptable-with-reasoning risks (from review; documented, not hidden)

- Indefinite wait: if pci.sys never completes a start (kernel-class
  unreliability), the result is a hang needing power-cycle — not a boot loop.
- The counter counts failed runs, not only crashes: three failed-but-clean
  transaction boots trip passivity until manual reset (fail-open direction).
- Retained path (firmware still running at failure): one ~90 MB + 16 MiB-VA
  leak set per boot until reboot, by upstream design (freeing live falcon
  DMA is worse).
- Early-boot file-system races may make a boot silently skip the unlock
  (fail-open no-op).

## Staged protocol (the only path to hardware)

- **Stage A (offline)** — rewrite, adversarial review, fixes, CI green.
  *Status: complete pending CI.*
- **Stage B (offline)** — this charter + RECOVERY-KIT.md published.
- **Stage C (on machine)** — toolchain restored; inert install; **two
  ordinary reboots with the filter attached and gate off, user watching**.
- **Stage D (on machine)** — one armed cycle with the recovery kit printed
  and on hand; benchmark verdict immediately; explicit leave-armed-or-disarm
  decision.
- **Any crash at any stage** — full stop, uninstall, offline root-cause.
