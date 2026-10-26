# GuFltUnlock activation (elevated): Enable=1 -> device cycle -> the filter
# holds the start IRP, runs the transaction serialized, releases nvlddmkm onto
# the result -> read LastRun* -> health -> benchmark. Screen blanks during the
# cycle; watchdog at 90 s re-enables unconditionally.
$ErrorActionPreference = 'Continue'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'runflt.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"
  $p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'

  # arm
  Set-ItemProperty $p -Name Enable -Value 1 -Type DWord
  Set-ItemProperty $p -Name LastRunState -Value 0 -Type DWord
  Write-Host "armed: Enable=1"

  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command","Start-Sleep -Seconds 90;",
    "pnputil /enable-device '$full' 2>`$null | Out-Null; exit")

  Write-Host "cycle: remove -> rescan (stack rebuild; filter holds the start)"
  pnputil /remove-device $full | Out-Null
  Start-Sleep 2
  pnputil /scan-devices | Out-Null

  # the start (and the transaction inside it) can take ~10-40 s; watch the marks
  $state = 0
  for ($j = 0; $j -lt 60; $j++) {
    Start-Sleep 2
    $state = (Get-ItemProperty $p -ErrorAction SilentlyContinue).LastRunState
    if ($state -eq 2 -or $state -eq 4) { break }
  }
  $props = Get-ItemProperty $p
  Write-Host ("filter marks: Attached={0} State={1} Reached={2} Verdict=0x{3:X8} ph2=0x{4:X8} ph7=0x{5:X8} ph12=0x{6:X8}" -f `
    $props.Attached, $props.LastRunState, $props.LastRunReached, $props.LastRunVerdict, `
    $props.LastRunPhase2, $props.LastRunPhase7, $props.LastRunPhase12)

  # settle + health + verdict before any reboot
  Start-Sleep 8
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  $healthy = $false
  try { & "$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe" --query-gpu=name --format=csv,noheader 2>&1 | Out-Null; $healthy = ($LASTEXITCODE -eq 0) } catch {}
  Write-Host "health: nvidia-smi=$healthy"
  # disarm for next boot (one-shot semantics)
  Set-ItemProperty $p -Name Enable -Value 0 -Type DWord
  if ($healthy) {
    Write-Host "verdict benchmark (baseline: FP16 30.24 / BF16 30.41):"
    & .\benchmark\gemm_bench.exe 2>&1 | Tee-Object (Join-Path $build 'post-unlock.txt')
  }
  Set-Content (Join-Path $build 'runflt-DONE') "state=$state healthy=$healthy"
} finally { Stop-Transcript }
