# gpu-unlock-rt · The full cycle with a planned smooth handover.
# Stages, each logged, each waiting for its successor's precondition:
#   0  pre-flight  : service deploy (stop->copy->sign->start)
#   1  warm        : cap-cache call while the card is enabled
#   2  displace    : pnputil /remove-device (screen blanks)
#   3  transact    : IOCTL PhaseLimit 12 (tower with corrected wake polls)
#   4  handover    : scan-devices -> WAIT for device Present -> WAIT driver bound
#                    -> settle 8 s (display recovery) -> nvidia-smi health gate
#   5  verdict     : gemm_bench vs baseline-locked.txt (BEFORE any reboot)
# Watchdog: 90 s hidden process, scan+enable as the last resort.
# If the desktop glitches after handover: Win+Ctrl+Shift+B restarts the display
# driver in-session. Reboot only after the verdict is captured.
$ErrorActionPreference = 'Continue'
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'run12.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"

  # ---- 0: service deploy -------------------------------------------------
  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) { if ((Get-Service GuRtUnlock).Status -eq 'Stopped') { break }; Start-Sleep 2 }
  Copy-Item .\build2\GuRtUnlock.sys .\build\GuRtUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build\GuRtUnlock.sys | Out-Null
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host ("stage0 svc: " + (Get-Service GuRtUnlock).Status + " binary=" + (Get-Item .\build\GuRtUnlock.sys).Length)

  # ---- locate target + watchdog ------------------------------------------
  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $loc = (Get-PnpDeviceProperty -InstanceId $full -KeyName 'DEVPKEY_Device_LocationInfo').Data
  if ($loc -notmatch '(\d+)\D+(\d+)\D+(\d+)') { throw "BDF parse: '$loc'" }
  $bus, $slot, $fn = [int]$Matches[1], [int]$Matches[2], [int]$Matches[3]
  Write-Host "stage0 bdf: $bus/$slot/$fn  instance: $full"
  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command",
    "Start-Sleep -Seconds 90;",
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
    $h = [GuIo]::CreateFile("\\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { return $false }
    $ok = [GuIo]::DeviceIoControl($h, 0x00222004, $in, $in.Length, $outbuf, $outbuf.Length, [ref]$ret, [IntPtr]::Zero)
    [GuIo]::CloseHandle($h) | Out-Null
    return $ok
  }

  # ---- 1: warm cap-cache (card enabled) -----------------------------------
  $wout = New-Object byte[] 108
  [void](Invoke-Tower 0 0 $wout)
  Write-Host ("stage1 warm: pcie=0x{0:X} pm=0x{1:X}" -f [BitConverter]::ToUInt32($wout,80), [BitConverter]::ToUInt32($wout,84))

  # ---- 2: displace ---------------------------------------------------------
  Write-Host "stage2 displace: removing device (screen blanks now)"
  pnputil /remove-device $full | Out-Null

  # ---- 3: transact ---------------------------------------------------------
  $out = New-Object byte[] 108
  $ok = Invoke-Tower 12 1 $out
  $verdict = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 92))   # Diag[7]: tower verdict
  $reached = [BitConverter]::ToUInt32($out, 4)
  $diag6   = [BitConverter]::ToUInt32($out, 88)
  Write-Host "stage3 transact: ok=$ok phaseReached=$reached wakeRung=$diag6 verdict=$verdict"
  $ph = 0..13 | ForEach-Object { '{0:X8}' -f [BitConverter]::ToUInt32($out, 8 + $_*4) }
  Write-Host "stage3 phases: $($ph -join ' ')"

  # ---- 4: smooth handover ---------------------------------------------------
  Write-Host "stage4 handover: scan + wait bind + settle"
  pnputil /scan-devices | Out-Null
  $bound = $false
  for ($j = 0; $j -lt 30; $j++) {
    Start-Sleep 1
    $d = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
    if ($d -and $d.Status -eq 'OK' -and $d.Present) { $bound = $true; break }
  }
  Write-Host "stage4 bind: bound=$bound after $($j+1)s; settling 8s"
  Start-Sleep 8
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  $smi = & "$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe" --query-gpu=name --format=csv,noheader 2>&1
  $healthy = ($LASTEXITCODE -eq 0)
  Write-Host "stage4 health: nvidia-smi=$healthy ($smi)"

  # ---- 5: verdict (before any reboot) ---------------------------------------
  if ($healthy) {
    Write-Host "stage5 verdict: benchmark (locked baseline: FP16 30.24 / BF16 30.41)"
    & .\benchmark\gemm_bench.exe 2>&1 | Tee-Object (Join-Path $build 'post-unlock.txt')
  } else {
    Write-Host "stage5 skipped: display stack not healthy - use Win+Ctrl+Shift+B, then run benchmark\gemm_bench.exe manually"
  }
  Set-Content (Join-Path $build 'run12-DONE') "reached=$reached verdict=$verdict healthy=$healthy"
} finally {
  Stop-Transcript
}
