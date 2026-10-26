# Redeploy the instrumented filter: sign build2flt binary, repoint the
# service ImagePath there (no file-replacement while loaded), reset marks.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'redeploy.log') -Force
try {
  Set-Location (Join-Path $root '..')
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build2flt\GuFltUnlock.sys
  if ($LASTEXITCODE) { throw "sign failed" }
  $bin = (Resolve-Path .\build2flt\GuFltUnlock.sys).Path
  sc.exe config GuFltUnlock binPath= $bin
  if ($LASTEXITCODE) { throw "sc config failed" }
  # reset instrumentation marks for a clean read after reboot
  $p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'
  foreach ($n in 'StartSeen','StartTarget','StartPassive','StartEnable','LastRunState','LastRunReached') {
    Remove-ItemProperty $p -Name $n -ErrorAction SilentlyContinue
  }
  Set-ItemProperty $p -Name Enable -Value 0 -Type DWord
  Write-Host "REDEPLOYED: binPath=$bin Enable=0 marks reset. Reboot to load the instrumented filter."
} finally { Stop-Transcript }
