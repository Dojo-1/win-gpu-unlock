# gpu-unlock-rt · rebind-race cycle (ported Linux-boot-window insight).
# arm watch (kernel thread, fail-closed on flag) -> remove-device (card sleeps)
# -> scan-devices (platform walks card D3cold -> D0 -> ... -> nvlddmkm GSP)
# -> watch thread catches the pre-GSP D0 window and fires the full tower
# -> poll mailbox -> health gate -> benchmark verdict before any reboot.
# Losing the race = WPR2-up refusal = normal Windows boot, no harm.
$ErrorActionPreference = 'Continue'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'run13.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"

  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) { if ((Get-Service GuRtUnlock).Status -eq 'Stopped') { break }; Start-Sleep 2 }
  Copy-Item .\build2\GuRtUnlock.sys .\build\GuRtUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build\GuRtUnlock.sys | Out-Null
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host ("svc: " + (Get-Service GuRtUnlock).Status + " size=" + (Get-Item .\build\GuRtUnlock.sys).Length)

  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $loc = (Get-PnpDeviceProperty -InstanceId $full -KeyName 'DEVPKEY_Device_LocationInfo').Data
  if ($loc -notmatch '(\d+)\D+(\d+)\D+(\d+)') { throw "BDF parse: '$loc'" }
  $bus, $slot, $fn = [int]$Matches[1], [int]$Matches[2], [int]$Matches[3]
  Write-Host "bdf: $bus/$slot/$fn"

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
  function Ioctl([uint32]$code, [byte[]]$inb, [byte[]]$outb) {
    $ret = 0
    $h = [GuIo]::CreateFile("\\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { return $null }
    $ok = [GuIo]::DeviceIoControl($h, $code, $inb, $inb.Length, $outb, $outb.Length, [ref]$ret, [IntPtr]::Zero)
    [GuIo]::CloseHandle($h) | Out-Null
    return $ok
  }
  $ARM  = [uint32]0x00222008   # CTL_CODE(0x22, 0x802, METHOD_BUFFERED, 0)
  $POLL = [uint32]0x0022200C   # CTL_CODE(0x22, 0x803, METHOD_BUFFERED, 0)

  # 1: arm the watch (requires the blackout flag - fail closed)
  $armIn = [BitConverter]::GetBytes([uint32]24) + [BitConverter]::GetBytes([uint32]$bus) +
           [BitConverter]::GetBytes([uint32]$slot) + [BitConverter]::GetBytes([uint32]$fn) +
           [BitConverter]::GetBytes([uint32]0) + [BitConverter]::GetBytes([uint32]1)
  $armOut = New-Object byte[] 8
  $armed = Ioctl $ARM $armIn $armOut
  Write-Host "watch armed: $armed"
  if (-not $armed) { throw "arm failed" }

  # 2: displace (screen blanks; card to D3cold - irrelevant now)
  Write-Host "displace: remove-device"
  pnputil /remove-device $full | Out-Null
  Start-Sleep 1

  # 3: rebind begins - the watch thread races for the D0 window
  Write-Host "rebind: scan-devices (watch races nvlddmkm GSP boot)"
  pnputil /scan-devices | Out-Null

  # 4: poll the mailbox (tower can take seconds: GFW 2.05s waits etc.)
  $pr = New-Object byte[] 112   # GU_WATCH_RESULT: 4+4+14*4+8*4 = 112... = 8+56+32=96? sizeof: State4+Phase4+Status56+Diag32 = 96
  $pr = New-Object byte[] 96
  $state = -1
  for ($j = 0; $j -lt 90; $j++) {
    Start-Sleep 1
    [void](Ioctl $POLL ([byte[]]@(1)) $pr)
    $state = [BitConverter]::ToUInt32($pr, 0)
    if ($state -eq 2 -or $state -eq 3 -or $state -eq 4) { break }
    if ($j % 5 -eq 0) { Write-Host "  watch state: $state ($j s)" }
  }
  $reached = [BitConverter]::ToUInt32($pr, 4)
  $verdict = [BitConverter]::ToUInt32($pr, 92)  # Diag[7] tower verdict
  $diag6   = [BitConverter]::ToUInt32($pr, 88)  # Diag[6]
  $ph = 0..13 | ForEach-Object { '{0:X8}' -f [BitConverter]::ToUInt32($pr, 8 + $_*4) }
  Write-Host "result: state=$state reached=$reached wakeRung=$diag6 verdict=0x$('{0:X8}' -f $verdict)"
  Write-Host "phases: $($ph -join ' ')"

  # 5: settle + health + verdict (before any reboot)
  Start-Sleep 8
  $healthy = $false
  try { & "$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe" --query-gpu=name --format=csv,noheader 2>&1 | Out-Null; $healthy = ($LASTEXITCODE -eq 0) } catch {}
  Write-Host "health: nvidia-smi=$healthy"
  if ($healthy) {
    Write-Host "verdict benchmark (baseline: FP16 30.24 / BF16 30.41):"
    & .\benchmark\gemm_bench.exe 2>&1 | Tee-Object (Join-Path $build 'post-unlock.txt')
  }
  Set-Content (Join-Path $build 'run13-DONE') "state=$state reached=$reached healthy=$healthy"
} finally { Stop-Transcript }
