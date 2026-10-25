param([Parameter(Mandatory=$true)][string]$Step)   # probe|phase3|phase12|cycle
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
switch ($Step) {
  'cycle' { sc.exe stop GuRtUnlock | Out-Null; Start-Sleep 2; sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
            Set-Content (Join-Path $build 'svc-status') (Get-Service GuRtUnlock).Status; exit }
}
Start-Transcript -Path (Join-Path $build "step-$Step.log") -Force
try {
  $svc = Get-Service GuRtUnlock -ErrorAction SilentlyContinue
  if ($svc.Status -ne 'Running') { sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2 }
  Write-Host "driver service: $((Get-Service GuRtUnlock).Status)"
  Set-Location $root\..
  switch ($Step) {
    'probe'  { .\orchestrate.ps1 -PhaseLimit 0 }
    'phase3' { .\orchestrate.ps1 -PhaseLimit 3 -ConfirmBlackout }
    'phase12'{ .\orchestrate.ps1 -PhaseLimit 12 -ConfirmBlackout }
  }
  Set-Content (Join-Path $build "step-$Step-DONE") "exit=$LASTEXITCODE"
} finally { Stop-Transcript }
