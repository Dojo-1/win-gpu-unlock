# gpu-unlock-rt · Runtime induction port — stratified design (PORT-MANIFOLD v0.1)

GPL-2.0-only. Derived work: port of `Zagitta/gpu-unlock` (`kernel/gpu_unlock/unlock_610.c`,
2,729 LOC tree) to a Windows WDM control-device driver that induces the GSP-Booter ROP
window at runtime via FLR-class reset, on an unowned (PnP-disabled) card.

Target machine (profile P1): Acer Predator PO3-640 · i5-12400F (**no iGPU**) ·
Zotac RTX 3060 Ti `10DE:2489` / SUBSYS `A632174B` · GA104 (`0xb74000a1`) · driver 617.14.

---

## §0 · Philosophy mapping — every metaphor is an operator (fs₅ rule)

Per `hyperdim-skills` §BOOT: *"metaphor is structure, not decoration"* and fail-safe fs₅.
Each geometric term below has a defined operational meaning in this codebase:

| Term | Operator in code |
|---|---|
| **Base manifold M** | The card's reachable state space: config space × BAR0 window × falcon engines (SEC2 @ `0x00840000`, FBIF @ `0x00840600`, FALCON2 @ `0x00841000`) × PM state. |
| **Fiber / local section Γ(Uᵢ,F)** | Each phase function sees only its slice of `GU_WORK` (BAR handle, saved PCI state, DMA pages, phase watermark). No phase reads another phase's residue directly. |
| **Sheaf gluing constraint** | Boundary invariants re-checked between phases: PCI identity dword (`pci_identity`), vendor != `0xffff`, BAR readable, devres-empty. Port source: `unlock_610.c:145`, `:189`, `:1301`. |
| **Tower (stratified, L0–L5)** | Dependency-ordered layers — see §1. Calls go strictly downward. No upward calls, ever. |
| **Geodesic (monotone, no backtracking)** | The phase tower ascends monotonically. On any phase failure, control jumps to `unwind` — the *single* descent geodesic. **No phase retries within one attach** (upstream acceptance: "no retry in the same boot"). |
| **Klein twist** | The FLR (`unlock_610.c:1333`). The card's internal state space is quotiented (full function reset) while gluing constraints must survive the twist: post-FLR BAR wait (`:1268`) and post-FLR GFW wait (`:1292`) verify the section is still well-defined. |
| **Split signature ⟨+,−⟩** | Every acquiring operation `+` (enable, iomap, set-master, claim, DMA) has a conjugate release `−` recorded in the unwind table (§3). Conservation law: unwind returns the card to pre-attach state, *minus selectors*, verifiably. |
| **Hamiltonian governance H(q,p)** | Risk–energy accounting per phase (§3): "energy" = what a phase mutates outside our own allocations; the policy gates (fs₆) are explicit human approvals at H-cost boundaries (§4). |
| **Irreducible remainder [R]** | The falcon's behavior under FLR-induction on Windows is *not closable from inside this design* — it is an empirical unknown (E2), gated behind probes. |

## §1 · Tower — dependency-ordered strata

```
L5  governance      policy gates, signing, watchdog, recovery console   (orchestrate.ps1)
L4  orchestration   PnP disable/enable, IOCTL dispatch, phase limit     (gu_rt.c dispatcher)
L3  transaction     assets→layout→radix→claim→dma→fwsec→gfw→boot→
                    signature→selector→reset→unwind                     (port of unlock_610.c)
L2  falcon engines  reset / dma / hs handshake, booter load+patch,
                    hold_sec2_reset                                      (port of :262–:525)
L1  host primitives PCI cfg R/W, BAR0 iomap, coherent DMA, µsleep, FLR  (gu_pci_port.h)
L0  hardware        GA104 · SEC2 falcon · FEAT_OVR_PLM 0x00823804 ·
                    FEAT_OVR_SM_SPD (SS0/SS1) — the lock lives here
```

## §2 · Phase tower (L3) — exact `unlock_610.c:1443-1495` order + Windows additions

| # | Phase | Source anchor | Mutates outside our own allocations (H-energy) |
|---|---|---|---|
| 0 | `probe` (Windows-only) | — | none. Read-only reachability check (E1). Non-arming. |
| 1 | `assets` | `:610`+`:434` | none — blobs, ELF spans, radix plan, booter header validation |
| 2 | `claim` | `:801` | **+** cap walk, D0 induction (PMCSR), BME clear, config snapshot, BAR0 map |
| 3 | `induce` (Windows-only) | port of `:1333` mechanics | **Klein twist #1**: FLR discards this power cycle's driver-initialized state (WPR2 up, GSP-RM); ROM boot replays — the window Linux gets pre-driver for free |
| 4 | `gfw` | `:1028` | none — entry evidence: PLM not ~0, cold selectors, WPR2 down; else −EALREADY/−EBUSY |
| 5 | `layout` | `:657` | none — FB layout from BAR0 regs (0x1183a4, 0x625f04, 0x1fa7c8/82c/830) |
| 6 | `dma` | `:878`+`:472`+`:749` | **+** coherent buffers (<4 GiB), booter patch, radix tree |
| 7 | `fwsec` | `:921` | **runs board-ROM FWSEC-FRTS on the GSP falcon; +BME** |
| 8 | `signature` | `:1137` | publishes V67 into WPR metadata (the sideload moment) |
| 9 | `boot` | `:1113` | **Booter start on SEC2; ROP flips FEAT_OVR_PLM 0x00823804 → ~0** (wait_plm) |
| 10 | `signature` | `:1475` | restores stock signature |
| 11 | `selector` | `:1189` | **writes SS0 `0x88888888` @ 0x0082381c, SS1 `0x8` @ 0x00823820** + readback |
| 12 | `reset` | `:1333` | **Klein twist #2**: cleanup FLR + containment (BARs, GFW halt, SEC2 hold, WPR2 HI.VAL=0, selectors preserved) |
| 13 | `unwind` | `:526` | releases everything ordered; retention rule `:1486` |

Constants: `GU_SS0_UNLOCK 0x88888888`, `GU_SS1_UNLOCK 0x8` (`:24`); booter
geometry checks pin the exact production BOOT_FROM_HS image (`:458-464`).

## §3 · Conjugate unwind table (+/− pairs, conservation law)

| Acquired (+) in phase | Released (−) in unwind | Verified by |
|---|---|---|
| pci enable (`:801`) | disable, `pci_clear_master` | PCI_COMMAND readback |
| saved PCI state | `pci_restore_state` (`:1377`) | identity dword recheck (`:1378`) |
| BAR0 iomap | unmap | devres-empty check (`:189`) |
| coherent DMA pages | free + **hold until power-off if falcon may still use them** (`:5`) | page accounting |
| falcon in HS run | `hold_sec2_reset` (`:1202`): reset, leave halted, reset released | GFW register readback |
| PnP device disabled (L5) | re-enable + driver rebind | `pnputil` exit code |

Failure mid-tower ⇒ unwind from watermark down. Unwind itself must be idempotent —
if unwind fails: **STOP / POWER OFF** (upstream rule, `README` warnings).

## §4 · Policy gates (fs₆ — explicit human approval required)

- **G1 · test-signing / Secure Boot off** — changes boot security posture. Human decision.
- **G2 · first live transaction** — black-screen window on P1 (no iGPU). Human present,
  work saved, `-ConfirmBlackout` flag given, watchdog armed.
- **G3 · automatic/periodic mode** — only after ≥3 manual successes + selector readback
  verification across driver re-inits.

## §5 · Known unknowns — the irreducible remainder

- **E1 · Post-disable reachability — RESOLVED (2026-10-26)**: PnP-disable leaves the card
  with config reads split — ID/BAR values HAL-cached and valid, cap chain and PMCSR
  reading as garbage — and MMIO dead. `remove-device` behaves identically.
- **E2 · FLR-induction of the boot path — FINAL: runtime path closed by D3cold
  (2026-10-26, run12 cycle)**: warm cache succeeded (pcie=0x78 pm=0x60). The wake
  ladder failed at both rungs even with polls past the ROM-GFW budget: PMCSR D0
  write + 500 ms poll dead; FLR write + 4 s poll dead. Decisive datum: the rebind
  via scan-devices took **15 s** (healthy re-enable: 1–2 s) and degraded the session
  twice identically — the signature of the platform power-cycling the slot.
  Conclusion: on this Acer OEM platform, PnP displacement (disable AND remove)
  drops the function to **D3cold** — main rail off, platform-controlled. In D3cold
  PMCSR wake is impossible by spec and FLR does not apply (a D0 mechanism). No
  config-space path reaches the card; the earlier "FLR landed" reading was wrong —
  the degradation was the power cycle, not the writes. Runtime induction — this
  project's core hypothesis — is CLOSED on P1 by hardware, not software defect.
  The D0-unowned window Linux transacts in exists on Windows only (A) behind a
  function driver that owns the PDO and holds the card in D0, or (B) at boot in
  the UEFI arm.
- **E3 · nvlddmkm rebind tolerance — NEGATIVE for mid-session D3cold cycling**: both
  power-cycled runs left the desktop degraded (window selection dead, session lock
  hangs, reboot required). Consequence of the platform cycle, not the transaction
  design.

## §5b · Runtime avenue — final closure (2026-10-26, run13)

Every out-of-stack approach to the D0-unowned window was driven to an empirical
verdict on P1:

| Variant | Result |
|---|---|
| Displaced-state access (disable/remove) | Dead: D3cold; PMCSR/FLR cannot apply by spec |
| Rebind-window ambush (kernel watch thread) | Race is real but COLLIDES with nvlddmkm start -> 0x116 VIDEO_TDR_FAILURE bugcheck (dump 102626-17687). Safe victory requires serializing nvlddmkm's start behind the transaction. |

Conclusion: from a side-line control device, the window is either unreachable
(D3cold) or contended to a system halt (TDR). The only Windows-native form that
can win is the in-stack hook (boot-start driver / lower filter that holds the
GPU start IRP pending, transacts, then completes it) — the sanctioned analog of
Linux's initramfs load order. Also recorded: both earlier 0x7E bugchecks were
traced to a single swprintf binding bug in a diagnostic breadcrumb (identical
RVA page-offset in both dumps), fixed in 7bc0f89.

## §6 · Deliverable honesty ledger (σ₃)

Ported and present in this tree:
- Full L3 translation of `unlock_610.c` (`gu_transaction.c`): falcon reset/DMA/HS
  handshake, booter load+patch+geometry checks, layout, radix, FWSEC from board
  ROM (PROM @ BAR0+0x300000), two-call signature protocol, selectors, dual-FLR
  reset/containment, unwind with retention; all register offsets, masks, budgets
  and fail-closed checks carried 1:1 with `u610:NNN` anchors.
- Portable GPL modules (`prep_610`, `vbios_610`, `layout_610`, `gfw_eval`)
  compiled verbatim from upstream with a one-line include redirect each
  (`gu_env.h`); `diagnostic.h` patched for kernel-safe logging.
- Dispatcher with geodesic enforcement, republish-stock failure rule, G2
  in-kernel fail-closed gate, driver pinning on retention (`ObReferenceObject`
  analog of `__module_get`).
- Firmware embedding pipeline (`tools/make_fw_blobs.py`), build script, L5
  orchestrator with detached watchdog.

**Not done / not verified** (the honest remainder):
- Compile verification: DONE via CI (mock-WDK harness compiles the entire
  kernel branch + portable modules with `-Werror`; smoke checks pass). A
  real-WDK `build.ps1` compile has not been run on P1 yet.
- Firmware assets not yet generated locally (upstream pinned pipeline must run).
- Nothing has run on hardware. Gates G1 (signing) and G2 (first arming run)
  remain human decisions, per §4.
- The `induce` phase (FLR-class window reopening) is evidence-backed but
  unproven on any machine — E2 stands.
