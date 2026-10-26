# GuFltUnlock installer (elevated). PASS-THROUGH install: the filter attaches
# on the next device cycle but Parameters\Enable=0 means it never intervenes.
# Removal = uninstall-filter.ps1 (reversible).
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'install-filter.log') -Force
try {
  Set-Location (Join-Path $root '..')
  # 1) deploy + sign the filter binary
  Copy-Item .\build2flt\GuFltUnlock.sys .\build\GuFltUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build\GuFltUnlock.sys
  if ($LASTEXITCODE) { throw "sign failed" }
  # 2) kernel service (demand start - loads when the stack needs it)
  if (Get-Service GuFltUnlock -ErrorAction SilentlyContinue) { sc.exe delete GuFltUnlock | Out-Null; Start-Sleep 2 }
  sc.exe create GuFltUnlock type= kernel start= demand binPath= "$((Resolve-Path .\build\GuFltUnlock.sys).Path)"
  if ($LASTEXITCODE) { throw "sc create failed" }
  # 3) gate OFF (pass-through)
  New-Item -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters' -Force | Out-Null
  Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters' -Name Enable -Value 0 -Type DWord

  # 4) find the 3060 Ti's class-key instance and APPEND our filter (preserve existing)
  $classKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
  $target = $null
  Get-ChildItem $classKey -ErrorAction SilentlyContinue | ForEach-Object {
    $props = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
    if ($props.MatchingDeviceId -like '*DEV_2489&SUBSYS_A632174B*' -or
        $props.DriverDesc -like '*3060 Ti*') { $target = $_ }
  }
  if (-not $target) { throw "3060 Ti class instance not found under $classKey" }
  Write-Host "device instance: $($target.PSChildName)"
  $cur = (Get-ItemProperty $target.PSPath -ErrorAction SilentlyContinue).LowerFilters
  if ($cur -contains 'GuFltUnlock') {
    Write-Host "LowerFilters already contains GuFltUnlock"
  } else {
    $new = @('GuFltUnlock') + @($cur | Where-Object { $_ })
    Set-ItemProperty $target.PSPath -Name LowerFilters -Value $new -Type MultiString
    Write-Host "LowerFilters set: $($new -join ', ')"
  }
  Write-Host 'INSTALLED (pass-through, Enable=0). Activation: runflt.ps1 (device cycle).'
} finally { Stop-Transcript }
