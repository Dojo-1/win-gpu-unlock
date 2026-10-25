# elevated: run DebugView capture + phase3 attempt
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
$dbg = Join-Path $root '..\tools-dbview\Dbgview.exe'
$cap  = Join-Path $build 'dbgcapture.log'
Remove-Item $cap -ErrorAction SilentlyContinue
$p = Start-Process $dbg -ArgumentList "/accepteula","/log",$cap -PassThru
Start-Sleep 3
Set-Location $root\..
.\orchestrate.ps1 -PhaseLimit 3 -ConfirmBlackout
Start-Sleep 2
Stop-Process $p -Force -ErrorAction SilentlyContinue
Set-Content (Join-Path $build 'dbg-run-DONE') 'ok'
