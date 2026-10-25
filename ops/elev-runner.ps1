# One-shot elevated command runner: executes the staged command, records output.
$ops  = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmdF = Join-Path $ops 'elev-cmd.txt'
$outF = Join-Path $ops 'elev-out.txt'
$rcF  = Join-Path $ops 'elev-rc.txt'
if (-not (Test-Path $cmdF)) { Set-Content $rcF 'no-command'; exit 1 }
$cmd = Get-Content $cmdF -Raw
Remove-Item $cmdF -Force
$out = powershell -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | Out-String
Set-Content $outF $out
Set-Content $rcF $LASTEXITCODE
