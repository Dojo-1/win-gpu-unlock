# Dry attach via class-level filter (survives re-enumeration) + remove/scan cycle.
# Gate OFF: the guarded filter attaches, passes every IRP, marks Attached/Bus.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'attach-check2.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"
  $p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'

  # class-level filter (device-key values get wiped on re-enumeration)
  $ck = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
  $cf = (Get-ItemProperty $ck -ErrorAction SilentlyContinue).LowerFilters
  if ($cf -notcontains 'GuFltUnlock') {
    Set-ItemProperty $ck -Name LowerFilters -Value (@('GuFltUnlock') + @($cf | Where-Object { $_ })) -Type MultiString
    Write-Host "class LowerFilters set"
  }

  # deploy the hardware-guarded build + sign
  Copy-Item .\build2flt\GuFltUnlock.sys .\build\GuFltUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f .\build\GuRtTest.pfx /p gu-rt-test .\build\GuFltUnlock.sys | Out-Null
  Write-Host "deployed + signed"

  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command","Start-Sleep -Seconds 120;",
    "pnputil /enable-device '$full' 2>`$null | Out-Null; exit")

  Write-Host "displace: remove-device (stack rebuilt on rescan)"
  pnputil /remove-device $full | Out-Null
  Start-Sleep 2
  Write-Host "rescan:"
  pnputil /scan-devices | Out-Null
  Start-Sleep 3
  $bound = $false
  for ($j = 0; $j -lt 40; $j++) {
    $d = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
    if ($d -and $d.Status -eq 'OK' -and $d.Present) { $bound = $true; break }
    Start-Sleep 1
  }
  Write-Host "bound=$bound after $($j+1)s; svc=$((Get-Service GuFltUnlock).Status)"
  Start-Sleep 8
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  $props = Get-ItemProperty $p
  Write-Host ("marks: Attached={0} Bus={1} LastRunState={2}" -f $props.Attached, $props.Bus, $props.LastRunState)
  Set-Content (Join-Path $build 'attach-check2-DONE') "bound=$bound attached=$($props.Attached) svc=$((Get-Service GuFltUnlock).Status)"
} finally { Stop-Transcript }
