# SPDX-License-Identifier: GPL-2.0-only
# gpu-unlock-rt · build + test-sign. Prerequisites (gate G1, PORT-MANIFOLD §4):
#   - LLVM 20.x on PATH or -Clang path (clang-cl + lld-link; WDK/msbuild also works)
#   - Windows SDK (for ntddk headers — ships with WDK; clang can use WDK include dirs)
#   - Firmware assets generated upstream: run ../gpu-unlock/firmware/{fetch_artifacts,generate_v67,extract_boot_assets}.py
#   - Test signing requires admin + testsigning enabled OR self-signed cert + SB off
# Usage: .\build.ps1 [-AssetDir ..\gpu-unlock\firmware\out] [-Clang C:\path\clang-cl.exe]
param(
    [string]$AssetDir = "",
    [string]$Clang = "clang-cl",
    [string]$LldLink = "lld-link"
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$drv = Join-Path $root 'driver'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

# 1) firmware blobs
if ($AssetDir -ne '') {
    python (Join-Path $root 'tools\make_fw_blobs.py') $AssetDir (Join-Path $drv 'gu_fw_blobs.h')
    if ($LASTEXITCODE) { throw "make_fw_blobs failed" }
} elseif (-not (Test-Path (Join-Path $drv 'gu_fw_blobs.h.generated'))) {
    # allow a pre-generated header to persist: presence check happens at compile
}

# 2) locate WDK include/lib (needed for ntddk.h / hal APIs)
$wdkRoots = @("$env:ProgramFiles(x86)\Windows Kits\10", "$env:ProgramFiles\Windows Kits\10")
$wdk = $null
foreach ($r in $wdkRoots) {
    if (Test-Path $r) {
        $inc = Get-ChildItem "$r\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1
        $lib = Get-ChildItem "$r\Lib"   -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($inc -and $lib) { $wdk = @{ Inc = $inc.FullName; Lib = $lib.FullName }; break }
    }
}
if (-not $wdk) { throw "WDK not found under Windows Kits\10 — install WDK (gate G1 prerequisite)." }
$kmInc = Join-Path $wdk.Inc 'km'
$ucrtInc = Join-Path $wdk.Inc 'ucrt'
$sharedInc = Join-Path $wdk.Inc 'shared'
$kmLibX64 = Join-Path $wdk.Lib 'km\x64'
Write-Host "WDK: $($wdk.Inc)"

# 3) compile: driver core + portable GPL modules
$sources = @(
    'gu_rt.c',
    'gu_transaction.c',
    'portable\prep_610.c',
    'portable\vbios_610.c',
    'portable\layout_610.c',
    'portable\gfw_eval.c'
)
$commonArgs = @(
    '/c', '/Zi', '/W4', '/WX-', '/GS-', '/GR-',
    '/D', '_AMD64_', '/D', '_WIN64', '/D', 'WIN32_LEAN_AND_MEAN',
    '/D', 'GU_WINDOWS_KERNEL=1',
    "/I$drv", "/I$kmInc", "/I$ucrtInc", "/I$sharedInc"
)
$objects = @()
foreach ($s in $sources) {
    $obj = Join-Path $out (([IO.Path]::GetFileNameWithoutExtension($s)) + '.obj')
    & $Clang @commonArgs (Join-Path $drv $s) "/Fo$obj"
    if ($LASTEXITCODE) { throw "compile failed: $s" }
    $objects += $obj
}

# 4) link a native driver image
& $LldLink @(
    '/DEBUG:FULL', '/MACHINE:X64', '/SUBSYSTEM:NATIVE,10', '/DRIVER',
    '/ENTRY:DriverEntry', '/NODEFAULTLIB',
    (Join-Path $kmLibX64 'ntoskrnl.lib')
) -OUT:(Join-Path $out 'GuRtUnlock.sys') @objects
if ($LASTEXITCODE) { throw "link failed" }
Write-Host "built: $out\GuRtUnlock.sys"

# 5) test-sign (gate G1): requires admin. Enable once per boot-config:
#   bcdedit /set testsigning on        (then reboot; Secure Boot must be OFF)
if (([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
     ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $cert = Join-Path $out 'GuRtTest.pfx'
    if (-not (Test-Path $cert)) {
        & signtool makecert /r /h 0 /n "CN=GuRtTest" /sv (Join-Path $out 'GuRtTest.pvk') (Join-Path $out 'GuRtTest.cer') *> $null
        & signtool pvk2pfx /pvk (Join-Path $out 'GuRtTest.pvk') /spc (Join-Path $out 'GuRtTest.cer') /pfx $cert *> $null
    }
    & signtool sign /fd SHA256 /f $cert (Join-Path $out 'GuRtUnlock.sys')
    Write-Host "signed: GuRtUnlock.sys (test cert $cert)"
    Write-Host "load:   sc create GuRtUnlock type= kernel binPath= $out\GuRtUnlock.sys && sc start GuRtUnlock"
} else {
    Write-Host "not admin: skipping test-sign (see header of this script)"
}
