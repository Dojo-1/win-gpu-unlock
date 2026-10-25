# SPDX-License-Identifier: GPL-2.0-only
# G1a (elevated): install WDK 10.0.26100 via winget. Marker + transcript in build\.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
Start-Transcript -Path (Join-Path $build 'g1a-transcript.log') -Force
try {
    winget install Microsoft.WindowsWDK.10.0.26100 `
        --accept-package-agreements --accept-source-agreements --silent
    $rc = $LASTEXITCODE
    Write-Host "winget exit: $rc"
    $km = "${env:ProgramFiles(x86)}\Windows Kits\10\Include\10.0.26100.0\km"
    if (Test-Path $km) {
        Write-Host "WDK km headers present: $km"
        Set-Content (Join-Path $build 'g1a-DONE') "ok $rc"
    } else {
        Write-Host "km headers missing after install"
        Set-Content (Join-Path $build 'g1a-FAILED') "km missing, winget $rc"
    }
} finally {
    Stop-Transcript
}
