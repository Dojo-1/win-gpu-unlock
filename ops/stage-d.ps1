# Stage D (elevated): THE armed cycle - the single gated run of the whole project.
# Prereqs (charter): recovery kit printed/in hand; Stage C green (two inert boots).
# Flow: arm Enable=1 -> remove-device -> scan-devices -> the filter HOLDS the GPU
# start IRP, runs the full transaction serialized at D0, releases nvlddmkm ->
# marks -> settle -> health -> benchmark verdict (if CUDA present) -> disarm.
# Watchdog 120 s (transaction budgets ~40 s worst case + rebind margin).
$ErrorActionPreference = 'Continue'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'stage-d.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"
  $p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'

  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  if (-not $dev) { throw "GPU not found" }
  $full = $dev.InstanceId
  Write-Host "target: $full"

  # arm (one-shot)
  Set-ItemProperty $p -Name Enable -Value 1 -Type DWord
  Set-ItemProperty $p -Name LastRunState -Value 0 -Type DWord
  Write-Host "armed: Enable=1 (disarms itself after the run)"

  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command","Start-Sleep -Seconds 120;",
    "pnputil /enable-device '$full' 2>`$null | Out-Null; exit")

  Write-Host "cycle: remove -> rescan (screen blanks; filter holds the start)"
  pnputil /remove-device $full | Out-Null
  Start-Sleep 2
  pnputil /scan-devices | Out-Null

  # watch the marks: State 2/4 or timeout
  $state = 0
  for ($j = 0; $j -lt 60; $j++) {
    Start-Sleep 2
    $state = (Get-ItemProperty $p -ErrorAction SilentlyContinue).LastRunState
    if ($state -eq 2 -or $state -eq 4) { break }
  }
  $props = Get-ItemProperty $p
  Write-Host ("RESULT: State={0} Reached={1} Verdict=0x{2:X8} ph2=0x{3:X8} ph4=0x{4:X8} ph7=0x{5:X8} ph9=0x{6:X8} ph12=0x{7:X8} BootCount={8}" -f `
    $props.LastRunState, $props.LastRunReached, $props.LastRunVerdict, `
    $props.LastRunPhase2, $props.LastRunPhase4, $props.LastRunPhase7, $props.LastRunPhase9, $props.LastRunPhase12, $props.BootCount)

  # settle + health
  $bound = $false
  for ($j = 0; $j -lt 40; $j++) {
    Start-Sleep 1
    $d = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
    if ($d -and $d.Status -eq 'OK' -and $d.Present) { $bound = $true; break }
  }
  Write-Host "rebind: bound=$bound after $($j+1)s; settling 10s"
  Start-Sleep 10
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  $healthy = $false
  try { & "$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe" --query-gpu=name --format=csv,noheader 2>&1 | Out-Null; $healthy = ($LASTEXITCODE -eq 0) } catch {}
  Write-Host "health: nvidia-smi=$healthy"

  # disarm regardless of outcome (one-shot semantics)
  Set-ItemProperty $p -Name Enable -Value 0 -Type DWord

  # verdict: benchmark if the stack is healthy and CUDA is present
  $bench = ".\benchmark\gemm_bench.exe"
  $cudaOk = Test-Path 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'
  if ($healthy -and (Test-Path $bench) -and $cudaOk) {
    $env:Path = "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6\bin;$env:Path"
    Write-Host "verdict benchmark (baseline: FP16 30.24 / BF16 30.41):"
    & $bench 2>&1 | Tee-Object (Join-Path $build 'post-run.txt')
  } elseif (-not $cudaOk) {
    Write-Host "CUDA absent: verdict from filter marks only; benchmark later"
  }

  Set-Content (Join-Path $build 'stage-d-DONE') "state=$state healthy=$healthy"
} finally { Stop-Transcript }
