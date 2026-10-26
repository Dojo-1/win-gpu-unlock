# Experiment cycle: (1) authoritative config dump while ENABLED (D0) via warm call
# with 364B buffer; (2) displaced CLS write test (PL=0, Flags=2) + full dump.
# Probe-class only: no tower, no PMCSR/FLR writes. Staged handover. Watchdog 90s.
$ErrorActionPreference = 'Continue'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'verdict2.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"

  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) { if ((Get-Service GuRtUnlock).Status -eq 'Stopped') { break }; Start-Sleep 2 }
  Copy-Item .\build2\GuRtUnlock.sys .\build\GuRtUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build\GuRtUnlock.sys | Out-Null
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host ("svc: " + (Get-Service GuRtUnlock).Status + " binary=" + (Get-Item .\build\GuRtUnlock.sys).Length)

  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $loc = (Get-PnpDeviceProperty -InstanceId $full -KeyName 'DEVPKEY_Device_LocationInfo').Data
  if ($loc -notmatch '(\d+)\D+(\d+)\D+(\d+)') { throw "BDF parse: '$loc'" }
  $bus, $slot, $fn = [int]$Matches[1], [int]$Matches[2], [int]$Matches[3]
  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command","Start-Sleep -Seconds 90;",
    "pnputil /scan-devices 2>`$null | Out-Null; pnputil /enable-device '$full' 2>`$null | Out-Null; exit")

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
  function Invoke-Tower([int]$phaseLimit, [int]$flags, [byte[]]$outbuf) {
    $in  = [BitConverter]::GetBytes([uint32]24) + [BitConverter]::GetBytes([uint32]$bus) +
           [BitConverter]::GetBytes([uint32]$slot) + [BitConverter]::GetBytes([uint32]$fn) +
           [BitConverter]::GetBytes([uint32]$phaseLimit) + [BitConverter]::GetBytes([uint32]$flags)
    $ret = 0
    $h = [GuIo]::CreateFile("\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { return $false }
    $ok = [GuIo]::DeviceIoControl($h, 0x00222004, $in, $in.Length, $outbuf, $outbuf.Length, [ref]$ret, [IntPtr]::Zero)
    [GuIo]::CloseHandle($h) | Out-Null
    return $ok
  }
  function Dump-Label([string]$label, [byte[]]$o) {
    $hex = ($o[112..363] | ForEach-Object { $_.ToString("X2") }) -join ''
    Write-Host "${label}: $hex"
  }

  # (1) warm 364B: authoritative cap chain + PMCSR at D0, card ENABLED
  $wout = New-Object byte[] 364
  [void](Invoke-Tower 0 0 $wout)
  Dump-Label "cfgD0" $wout
  Write-Host ("warm caps: pcie=0x{0:X} pm=0x{1:X}" -f [BitConverter]::ToUInt32($wout,80), [BitConverter]::ToUInt32($wout,84))

  # (2) displaced CLS test + displaced dump
  Write-Host "displace: remove-device (screen blanks)"
  pnputil /remove-device $full | Out-Null
  $out = New-Object byte[] 364
  $ok = Invoke-Tower 0 2 $out
  Write-Host ("CLS test: ok={0} Diag6={1} (20=writes land/D3hot, 21=dead/D3cold, 22/23=path fail)" -f $ok, [BitConverter]::ToUInt32($out,88))
  Dump-Label "cfgDisplaced" $out

  # staged handover
  pnputil /scan-devices | Out-Null
  $bound = $false
  for ($j = 0; $j -lt 30; $j++) {
    Start-Sleep 1
    $d = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
    if ($d -and $d.Status -eq 'OK' -and $d.Present) { $bound = $true; break }
  }
  Write-Host "handover: bound=$bound after $($j+1)s; settling 8s"
  Start-Sleep 8
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  $healthy = $false
  try { & "$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe" --query-gpu=name --format=csv,noheader 2>&1 | Out-Null; $healthy = ($LASTEXITCODE -eq 0) } catch {}
  Write-Host "health: nvidia-smi=$healthy"
  Set-Content (Join-Path $build 'verdict2-DONE') "clsDone bound=$bound healthy=$healthy"
} finally { Stop-Transcript }
