# G1c (elevated): enable test signing + register the kernel service.
$build = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..\build'
Start-Transcript -Path (Join-Path $build 'g1c-transcript.log') -Force
bcdedit /set testsigning on
$tsOk = $?
sc.exe create GuRtUnlock type= kernel start= demand binPath= "$((Join-Path $build 'GuRtUnlock.sys'))"
$scOk = $?
Set-Content (Join-Path $build 'g1c-DONE') "testsigning=$tsOk service=$scOk"
Stop-Transcript
