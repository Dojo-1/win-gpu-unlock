param([Parameter(Mandatory=$true)][ValidateSet('probe','phase3','phase12')]$Step)
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build "step-$Step.log") -Force
try {
  # cycle: the control device is exclusive; stale handles from aborted runs block opens
  sc.exe stop GuRtUnlock 2>$null | Out-Null; Start-Sleep 1
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  $svc = Get-Service GuRtUnlock
  Write-Host "driver service: $($svc.Status)"
  if ($svc.Status -ne 'Running') { Set-Content (Join-Path $build "step-$Step-FAILED") 'service not running'; return }
  Set-Location $root\..
  switch ($Step) {
    'probe'  { .\orchestrate.ps1 -PhaseLimit 0 }
    'phase3' { .\orchestrate.ps1 -PhaseLimit 3 -ConfirmBlackout }
    'phase12'{ .\orchestrate.ps1 -PhaseLimit 12 -ConfirmBlackout }
  }
  Set-Content (Join-Path $build "step-$Step-DONE") "orchestrate returned $LASTEXITCODE"
} finally { Stop-Transcript }
