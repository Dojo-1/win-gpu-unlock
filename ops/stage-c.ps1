# Stage C (elevated): INERT install of the generation-2 filter.
# - signs GuFltUnlock with the project test cert
# - registers the kernel service (demand start)
# - appends GuFltUnlock to the DISPLAY CLASS LowerFilters (survives re-enum)
# - Enable=0 (pass-through) and BootCount=0 (crash counter armed fresh)
# After this: TWO ordinary reboots, user watching. The filter attaches,
# holds nothing, marks Attached/BootCount. Nothing can intervene.
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'stage-c.log') -Force
try {
  Set-Location (Join-Path $root '..')

  # 1) deploy + sign
  Copy-Item .\build2flt\GuFltUnlock.sys .\build\GuFltUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build\GuFltUnlock.sys
  if ($LASTEXITCODE) { throw "sign failed" }
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" verify /pa .\build\GuFltUnlock.sys 2>&1 | Select-Object -First 1

  # 2) service + parameters (gate OFF, counter fresh)
  if (Get-Service GuFltUnlock -ErrorAction SilentlyContinue) { sc.exe delete GuFltUnlock | Out-Null; Start-Sleep 2 }
  sc.exe create GuFltUnlock type= kernel start= demand binPath= "$((Resolve-Path .\build\GuFltUnlock.sys).Path)"
  if ($LASTEXITCODE) { throw "sc create failed" }
  New-Item -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters' -Force | Out-Null
  Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters' -Name Enable -Value 0 -Type DWord
  Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters' -Name BootCount -Value 0 -Type DWord

  # 3) class-level LowerFilters (append, preserve)
  $ck = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
  $cf = (Get-ItemProperty $ck -ErrorAction SilentlyContinue).LowerFilters
  if ($cf -notcontains 'GuFltUnlock') {
    Set-ItemProperty $ck -Name LowerFilters -Value (@('GuFltUnlock') + @($cf | Where-Object { $_ })) -Type MultiString
  }
  Write-Host "STAGE C INSTALLED (inert): Enable=0, BootCount=0, class filter set."
  Write-Host "Next: reboot TWICE normally; then run ops\stage-c-verify.ps1"
} finally { Stop-Transcript }
