# One elevated window: rebuild (IRP fix), re-sign, cycle service, run probe.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'post-reboot.log') -Force
try {
  Set-Location $root\..
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -NoSign
  if ($LASTEXITCODE) { Set-Content (Join-Path $build 'post-reboot-FAILED') 'build'; return }
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test (Join-Path $build 'GuRtUnlock.sys')
  if ($LASTEXITCODE) { Set-Content (Join-Path $build 'post-reboot-FAILED') 'sign'; return }
  sc.exe stop GuRtUnlock 2>$null | Out-Null; Start-Sleep 2
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host "driver service: $((Get-Service GuRtUnlock).Status)"
  .\orchestrate.ps1 -PhaseLimit 0
  Set-Content (Join-Path $build 'post-reboot-DONE') "probe exit=$LASTEXITCODE"
} finally { Stop-Transcript }
