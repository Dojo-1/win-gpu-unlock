# Dry attach: device cycle with Enable=0 - the filter must attach, pass every
# IRP through untouched, and mark Attached/Bus. No transaction can run.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'attach-check.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"
  $p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'
  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command","Start-Sleep -Seconds 90;",
    "pnputil /enable-device '$full' 2>`$null | Out-Null; exit")
  Write-Host "cycle: disable -> enable (gate OFF, pass-through)"
  pnputil /disable-device $full | Out-Null
  Start-Sleep 2
  pnputil /enable-device $full | Out-Null
  $bound = $false
  for ($j = 0; $j -lt 30; $j++) {
    Start-Sleep 1
    $d = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
    if ($d -and $d.Status -eq 'OK' -and $d.Present) { $bound = $true; break }
  }
  Write-Host "handover: bound=$bound after $($j+1)s"
  Start-Sleep 8
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  $props = Get-ItemProperty $p
  Write-Host ("marks: Attached={0} Bus={1} Enable={2} LastRunState={3}" -f $props.Attached, $props.Bus, $props.Enable, $props.LastRunState)
  $svc = (Get-Service GuFltUnlock).Status
  Write-Host "filter service after cycle: $svc"
  Set-Content (Join-Path $build 'attach-check-DONE') "bound=$bound attached=$($props.Attached) svc=$svc"
} finally { Stop-Transcript }
