# Elevated chain: stop service -> rebuild -> sign -> start -> phase3 attempt.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'cycle-run3.log') -Force
try {
  Set-Location (Join-Path $root '..')

  # release the driver image before linking (running driver locks the .sys)
  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) {
    $st = (Get-Service GuRtUnlock).Status
    if ($st -eq 'Stopped') { break }
    Start-Sleep 2
  }
  Write-Host "svc after stop: $((Get-Service GuRtUnlock).Status)"

  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -NoSign
  if ($LASTEXITCODE) { Set-Content (Join-Path $build 'cycle-run3-FAILED') 'build'; return }

  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test (Join-Path $build 'GuRtUnlock.sys') | Out-Null
  if ($LASTEXITCODE) { Set-Content (Join-Path $build 'cycle-run3-FAILED') 'sign'; return }

  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host "svc after start: $((Get-Service GuRtUnlock).Status)"

  Set-Content (Join-Path $build 'cycle-run3-DONE') "built+started"
} finally { Stop-Transcript }
