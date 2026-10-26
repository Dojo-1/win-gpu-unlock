# Security & scope statement

## What this software does — and refuses to do

- **Card-locked by construction.** Every code path that touches hardware
  verifies PCI vendor `10DE`, device `2489`, and subsystem `A632174B` (Zotac
  RTX 3060 Ti). Any other card is refused fail-open. There is no
  configuration option to widen this — porting to another card is a source
  change, reviewed like any other.
- **No persistence without you.** The filter installs pass-through
  (`Parameters\Enable = 0`) and only ever transacts when explicitly armed.
  Every arming step requires administrator elevation and an interactive
  decision.
- **No firmware flashing, no bootloader changes, no partition edits.** The
  complete footprint is: one kernel service, one registry class value, one
  parameters key, one `.sys` file. All four are removable from WinRE in
  under five minutes (`docs/RECOVERY-KIT.md`).
- **Boot-loop-proof by design.** `DriverEntry`'s first statement durably
  increments a crash counter; three bad loads and the driver goes permanently
  passive. The worst arithmetically possible outcome is three crash-screens,
  then normal boots forever.

## Supply-chain posture

- **No binaries in this repository.** Source only. The firmware assets the
  driver embeds are NVIDIA proprietary and are *generated locally* by the
  hash-pinned upstream pipeline (`gpu-unlock/firmware/*.py`) — they are
  gitignored and must never be committed (enforced; history was purged and
  rewritten once already to remove an early leak).
- **No secrets.** The `gu-rt-test` password in ops scripts belongs to a
  throwaway self-signed test certificate created on your own machine. Nothing
  in this repo is a credential worth anything off your machine.
- **Test-signing is your decision and your exposure.** Loading the driver
  requires Windows test-signing mode (or your own signing). Test-signing
  weakens driver-signature enforcement system-wide — enable it only when you
  are doing this work, and `bcdedit /set testsigning off` when you stop.

## Reporting

Open an issue. Kernel drivers that touch GPU firmware sequencing are
inherently risky; describe hardware, driver version, and the
`Parameters\LastRun*` marks, and mark it **safety** if a machine would not
boot.
