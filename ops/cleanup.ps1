$build = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\build'
Start-Transcript -Path (Join-Path $build 'cleanup.log') -Force
# kill orphaned orchestrate/run-step powershells (they hold the exclusive device)
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
  Where-Object { $_.CommandLine -match 'run-step|orchestrate' } |
  ForEach-Object { Write-Host "killing $($_.ProcessId): $($_.CommandLine.Substring(0,[Math]::Min(90,$_.CommandLine.Length)))"; Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep 2
sc.exe stop GuRtUnlock | Out-Null
Start-Sleep 3
sc.exe start GuRtUnlock | Out-Null
Start-Sleep 2
(Get-Service GuRtUnlock).Status | Set-Content (Join-Path $build 'cleanup-DONE')
Stop-Transcript
