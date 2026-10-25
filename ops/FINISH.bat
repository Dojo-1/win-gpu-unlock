@echo off
rem gpu-unlock-rt · the whole remaining sequence in one right-click.
rem Right-click this file -> "Run as administrator". Expected: brief black
rem screens (watchdog re-enables at +45s worst case), then the verdict vs
rem the locked baseline (FP16 30.24 / BF16 30.41 TFLOPS).
cd /d "%~dp0.."
echo === phase 3: deploy cap-cache driver + claim/induce ===
powershell -NoProfile -ExecutionPolicy Bypass -File ops\swap-run.ps1
if not exist build\swap-run-DONE (
  echo phase 3 did not complete - see build\swap-run.log
  pause & exit /b 1
)
echo.
echo === phase 12: full transaction (G2) ===
powershell -NoProfile -ExecutionPolicy Bypass -File ops\step.ps1 -Step phase12
echo.
echo === verdict: post-unlock benchmark vs locked baseline ===
benchmark\gemm_bench.exe
echo.
type build\baseline-locked.txt 2>nul
echo.
pause
