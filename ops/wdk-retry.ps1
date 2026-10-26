$build = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$log = Join-Path $build 'build' 'wdk-retry.log'
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
Start-Transcript -Path $log -Force
winget install --id Microsoft.WindowsWDK.10.0.26100 --silent --accept-package-agreements --accept-source-agreements
"exit=$LASTEXITCODE" | Set-Content (Join-Path $build 'build' 'wdk-retry-DONE')
Stop-Transcript
