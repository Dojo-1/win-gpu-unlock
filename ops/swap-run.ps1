# Minimal elevated swap: stop service, deploy build2 binary, sign, start, phase3.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'swap-run.log') -Force
try {
  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) { if ((Get-Service GuRtUnlock).Status -eq 'Stopped') { break }; Start-Sleep 2 }
  Write-Host "stop: $((Get-Service GuRtUnlock).Status)"
  Copy-Item (Join-Path $root '..\build2\GuRtUnlock.sys') (Join-Path $build 'GuRtUnlock.sys') -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test (Join-Path $build 'GuRtUnlock.sys') | Out-Null
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host "start: $((Get-Service GuRtUnlock).Status) binary=$((Get-Item (Join-Path $build 'GuRtUnlock.sys')).Length)"
  Set-Location (Join-Path $root '..')
  .\orchestrate.ps1 -PhaseLimit 3 -ConfirmBlackout
  Set-Content (Join-Path $build 'swap-run-DONE') 'ok'
} finally { Stop-Transcript }
