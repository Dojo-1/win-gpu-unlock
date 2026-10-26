$build = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
Start-Transcript -Path (Join-Path $build 'build' 'cuda-retry.log') -Force
winget install --id Nvidia.CUDA --silent --accept-package-agreements --accept-source-agreements
"exit=$LASTEXITCODE" | Set-Content (Join-Path $build 'build' 'cuda-retry-DONE')
Stop-Transcript
