# SPDX-License-Identifier: GPL-2.0-only
# gpu-unlock-rt · L5 orchestrator: PnP displacement -> IOCTL tower -> rebind.
# Policy gates per PORT-MANIFOLD.md §4:
#   - PhaseLimit > 0 (probe) requires -ConfirmBlackout  [gate G2]
#   - watchdog re-enables the device unconditionally at deadline (descent geodesic
#     at the L5 stratum — the console MUST come back even if the driver hangs)
# Phase tower (driver/gu_rt.h): 0 probe · 1 assets · 2 claim · 3 induce · 4 gfw ·
#   5 layout · 6 dma · 7 fwsec · 8 signature · 9 boot · 10 signature2 ·
#   11 selector · 12 reset. Full run = -PhaseLimit 12.
# Usage:
#   .\orchestrate.ps1 -PhaseLimit 0                      # E1 probe, screen stays on
#   .\orchestrate.ps1 -PhaseLimit 2 -ConfirmBlackout     # through claim (D0/BAR check)
#   .\orchestrate.ps1 -PhaseLimit 12 -ConfirmBlackout    # full run (G2) — screen blanks
param(
    [string]$InstanceId = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1", # P1 profile
    [int]$PhaseLimit = 0,
    [switch]$ConfirmBlackout,
    [int]$WatchdogSec = 45   # tower worst case: 2x GFW waits (2.05s each) + FWSEC + 2x FLR + margins
)

$ErrorActionPreference = 'Stop'
if ($PhaseLimit -gt 0 -and -not $ConfirmBlackout) {
    throw "Gate G2: arming phases require -ConfirmBlackout (screen will blank; no iGPU)."
}

Add-Type -TypeDefinition @"
using System;using System.Runtime.InteropServices;
public static class GuIo {
  [DllImport("kernel32",SetLastError=true,CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateFile(string n,uint a,uint s,IntPtr sa,uint c,uint f,IntPtr t);
  [DllImport("kernel32",SetLastError=true)]
  public static extern bool DeviceIoControl(IntPtr h,uint c,byte[] inb,int inlen,byte[] outb,int outlen,out int ret,IntPtr o);
  [DllImport("kernel32")] public static extern bool CloseHandle(IntPtr h);
}
"@

# --- locate the full instance path (suffix digits vary per boot) ---
$dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$InstanceId*" -and $_.Class -eq 'Display' } |
       Select-Object -First 1
if (-not $dev) { throw "Target GPU not found for $InstanceId" }
$full = $dev.InstanceId
Write-Host "target : $full  (status: $($dev.Status))"

# --- derive BDF from the device's PCI location info ---
$loc = (Get-PnpDeviceProperty -InstanceId $full -KeyName 'DEVPKEY_Device_LocationInfo').Data
# e.g. "PCI Bus 1, Device 0, Function 0"
# locale-agnostic: three numbers in 'Bus PCI X, <word> Y, <word> Z'
if ($loc -notmatch '(\d+)\D+(\d+)\D+(\d+)') {
    throw "cannot parse BDF from location info: '$loc'"
}
$bus, $slot, $fn = [int]$Matches[1], [int]$Matches[2], [int]$Matches[3]
Write-Host "bdf    : bus $bus device $slot function $fn"

# --- watchdog: unconditional re-enable at deadline. Fires from a detached
#     process so it survives even if this script's host is starved. ---
$wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command",
    "Start-Sleep -Seconds $WatchdogSec;",
    "pnputil /enable-device '$full' 2>`$null | Out-Null;",
    "exit")

try {
    Write-Host "disable: detaching nvlddmkm (console may blank NOW if this is the only GPU)"
    pnputil /disable-device $full | Out-Null

    $h = [GuIo]::CreateFile("\\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { throw "open \\.\GuRtUnlock failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error()) (driver loaded? signed?)" }

    # GU_RUN_IN: Size,Bus,Device,Fn,PhaseLimit,Flags
    $in = [BitConverter]::GetBytes([uint32]24) + [BitConverter]::GetBytes([uint32]$bus) +
          [BitConverter]::GetBytes([uint32]$slot) + [BitConverter]::GetBytes([uint32]$fn) +
          [BitConverter]::GetBytes([uint32]$PhaseLimit) +
          [BitConverter]::GetBytes([uint32]$(if ($ConfirmBlackout) {1} else {0}))
    # GU_RUN_OUT: 8 + 13*4 + 8*4 + 4 + 8 = 92 -> pad to 96
    $out = New-Object byte[] 108
    $ret = 0
    $ok = [GuIo]::DeviceIoControl($h, 0x00222004, $in, $in.Length, $out, $out.Length, [ref]$ret, [IntPtr]::Zero)
    [GuIo]::CloseHandle($h) | Out-Null
    if (-not $ok) { throw "IOCTL failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $reached = [BitConverter]::ToUInt32($out, 4)
    Write-Host "tower  : phase reached $reached, diag id=$(('0x{0:X8}' -f [BitConverter]::ToUInt32($out,64))) bar0=$(('0x{0:X8}' -f [BitConverter]::ToUInt32($out,68))) chip=$(('0x{0:X8}' -f [BitConverter]::ToUInt32($out,72))) pm=$(('0x{0:X8}' -f [BitConverter]::ToUInt32($out,76)))"
}
finally {
    Write-Host "enable : rebinding driver (watchdog covers us at +$WatchdogSec s)"
    pnputil /enable-device $full | Out-Null
    if (-not $wd.HasExited) { Stop-Process $wd -Force }
    Get-PnpDevice -InstanceId $full | Format-List FriendlyName, Status | Out-Host
}
