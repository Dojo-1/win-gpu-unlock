# Local mock pass: compile the full kernel branch + gen-2 filter against the
# host mocks with MSVC (real SEH for the filter TU), link, run smoke checks.
# No elevation, no hardware.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location (Join-Path $root '..')
$out = Join-Path $root 'build-mock'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars  = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$inc     = "-Itests\host_harness -Idriver"
$flags   = "/nologo /c /Zi /W4 /D_AMD64_ /D_WIN64 /DWIN32_LEAN_AND_MEAN /DGU_WINDOWS_KERNEL=1"

$msvcDir = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207'
$kitDir  = 'C:\Program Files (x86)\Windows Kits\10'
$sdkV    = '10.0.26100.0'
$lines = @(('set "INCLUDE={0}\include;{1}\Include\{2}\ucrt;{1}\Include\{2}\um;{1}\Include\{2}\shared"' -f $msvcDir, $kitDir, $sdkV),
          ('set "LIB={0}\lib\x64;{1}\Lib\{2}\ucrt\x64;{1}\Lib\{2}\um\x64"' -f $msvcDir, $kitDir, $sdkV),
          ('set "PATH={0}\bin\Hostx64\x64;C:\Windows\System32;C:\Windows"' -f $msvcDir))
foreach ($s in @('tests\host_harness\harness.c','driver\gu_rt.c','driver\gu_gsp.c','driver\gu_transaction.c',
                 'driver\portable\prep_610.c','driver\portable\vbios_610.c','driver\portable\layout_610.c','driver\portable\gfw_eval.c')) {
  $obj = Join-Path $out (([IO.Path]::GetFileNameWithoutExtension($s)) + '.obj')
  $lines += "cl $flags $inc /Fo`"$obj`" `"$s`" || exit /b 1"
}
# gen-2 filter as its own TU (DriverEntry clash with the control driver)
$lines += "cl $flags $inc /Fo`"$out\gu_flt.obj`" driver\gu_flt.c || exit /b 1"
$lines += "link /nologo /DEBUG /MACHINE:X64 /OUT:`"$out\harness.exe`" /SUBSYSTEM:CONSOLE " +
          "$out\harness.obj $out\gu_rt.obj $out\gu_gsp.obj $out\gu_transaction.obj $out\prep_610.obj $out\vbios_610.obj $out\layout_610.obj $out\gfw_eval.obj || exit /b 1"
$cmd = Join-Path $out 'mock.cmd'
Set-Content -Path $cmd -Value ($lines -join "`r`n")
& cmd.exe /c $cmd
if ($LASTEXITCODE) { "MOCK BUILD FAILED"; exit 1 }
"=== smoke run ==="
& (Join-Path $out 'harness.exe')
"mock exit=$LASTEXITCODE"
