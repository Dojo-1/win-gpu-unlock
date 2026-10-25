# SPDX-License-Identifier: GPL-2.0-only
# gpu-unlock-rt · build + test-sign (MSVC/VS2022 + WDK headers/libs).
# Prerequisites:
#   - VS2022 with C++ toolset (cl at VC\Tools\MSVC\*\bin\Hostx64\x64)
#   - WDK 10 (winget install Microsoft.WindowsWDK.10.0.26100)  [gate G1 prerequisite]
#   - driver/gu_fw_blobs.h generated: python tools\make_fw_blobs.py <asset_dir> driver\gu_fw_blobs.h
#   - GSP image deployed: copy gsp_ga10x.bin -> C:\Windows\System32\drivers\GuRtGsp.bin (admin)
# Usage: .\build.ps1 [-NoSign]
param([switch]$NoSign)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$drv  = Join-Path $root 'driver'
$out  = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not (Test-Path (Join-Path $drv 'gu_fw_blobs.h'))) {
    throw "driver/gu_fw_blobs.h missing - run tools/make_fw_blobs.py first"
}

# --- locate VS2022 cl + WDK 26100 ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath  = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "VS2022 C++ toolset not found" }
$vcvars  = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$msvcToolset = (Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') | Sort-Object Name -Descending | Select-Object -First 1).FullName
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdk  = Get-ChildItem "$kits\Include" -Directory | Where-Object Name -eq '10.0.26100.0' | Select-Object -First 1
if (-not $sdk -or -not (Test-Path "$($sdk.FullName)\km")) { throw "WDK 10.0.26100 not installed (winget install Microsoft.WindowsWDK.10.0.26100)" }
$inc = $sdk.FullName
Write-Host "MSVC: $msvcToolset"
Write-Host "WDK : $inc"

$sources = @('gu_rt.c','gu_transaction.c','gu_gsp.c',
             'portable\prep_610.c','portable\vbios_610.c','portable\layout_610.c','portable\gfw_eval.c')
$includes = "/I`"$drv`" /I`"$inc\km`" /I`"$inc\km\crt`" /I`"$inc\shared`" /I`"$inc\ucrt`" /I`"$inc\um`""
$flags = "/nologo /c /kernel /Zi /W4 /GS- /GR- /Gz /D_AMD64_ /D_WIN64 /DWIN32_LEAN_AND_MEAN /DGU_WINDOWS_KERNEL=1"

$cl_lines = @("call `"$vcvars`"")
foreach ($s in $sources) {
    $obj = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($s) + '.obj')
    $cl_lines += "cl $flags $includes /Fo`"$obj`" `"$drv\$s`" || exit /b 1"
}
$kmLib = "$kits\Lib\10.0.26100.0\km\x64"
$cl_lines += "link /nologo /DEBUG:FULL /MACHINE:X64 /SUBSYSTEM:NATIVE,10 /DRIVER /ENTRY:DriverEntry " +
             "/OUT:`"$out\GuRtUnlock.sys`" /PDB:`"$out\GuRtUnlock.pdb`" `"$kmLib\ntoskrnl.lib`" `"$kmLib\hal.lib`" " +
             (($sources | ForEach-Object { "`"$out\$(Split-Path $_ -Leaf).obj`"" }) -join ' ').Replace('.c.obj','.obj') + " || exit /b 1"

$cmdFile = Join-Path $out 'build.cmd'
Set-Content -Path $cmdFile -Value ($cl_lines -join "`r`n")
& cmd.exe /c $cmdFile
if ($LASTEXITCODE) { throw "build failed" }
Get-Item "$out\GuRtUnlock.sys" | Select-Object Name, Length

if ($NoSign) { return }

# --- test-sign (gate G1): admin required ---
if (([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
     ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $cert = "$out\GuRtTest.pfx"
    if (-not (Test-Path $cert)) {
        $pw = ConvertTo-SecureString 'gu-rt-test' -AsPlainText -Force
        New-SelfSignedCertificate -Subject 'CN=GuRtTest' -Type CodeSigningCert -CertStoreLocation 'Cert:\CurrentUser\My' |
            ForEach-Object { Export-PfxCertificate -Cert $_ -FilePath $cert -Password $pw }
    }
    $signtool = "$kits\bin\10.0.26100.0\x64\signtool.exe"
    & $signtool sign /fd SHA256 /f $cert /p gu-rt-test "$out\GuRtUnlock.sys"
    Write-Host "signed. Enable once: bcdedit /set testsigning on  (Secure Boot must be OFF), then reboot."
} else {
    Write-Host "not elevated: skipping sign (run from admin shell for G1)"
}
