# Fully-checked deploy + link diagnosis + experiments, with signature verify.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'verdict4.log') -Force
try {
  Set-Location (Join-Path $root '..')
  $id = "PCI\VEN_10DE&DEV_2489&SUBSYS_A632174B&REV_A1"
  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) { if ((Get-Service GuRtUnlock).Status -eq 'Stopped') { break }; Start-Sleep 2 }
  Copy-Item .\build2\GuRtUnlock.sys .\build\GuRtUnlock.sys -Force
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test .\build\GuRtUnlock.sys
  Write-Host ("sign rc: " + $LASTEXITCODE)
  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" verify /pa .\build\GuRtUnlock.sys 2>&1 | Select-Object -First 2 | ForEach-Object { Write-Host "verify: $_" }
  sc.exe start GuRtUnlock; Write-Host ("start rc: " + $LASTEXITCODE)
  Start-Sleep 2
  Write-Host ("svc: " + (Get-Service GuRtUnlock).Status + " size=" + (Get-Item .\build\GuRtUnlock.sys).Length)
  try { Write-Host ("EntryStage: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\GuRtUnlock\Parameters' -ErrorAction Stop).EntryStage) } catch { Write-Host "EntryStage: <none>" }

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
  function T([int]$pl,[int]$flags,[int]$size) {
    $in  = [BitConverter]::GetBytes([uint32]24) + [BitConverter]::GetBytes([uint32]1) +
           [BitConverter]::GetBytes([uint32]0) + [BitConverter]::GetBytes([uint32]0) +
           [BitConverter]::GetBytes([uint32]$pl) + [BitConverter]::GetBytes([uint32]$flags)
    $out = New-Object byte[] $size; $ret = 0
    $h = [GuIo]::CreateFile("\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { return "open-fail err=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $ok = [GuIo]::DeviceIoControl($h, 0x00222004, $in, $in.Length, $out, $out.Length, [ref]$ret, [IntPtr]::Zero)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [GuIo]::CloseHandle($h) | Out-Null
    $sz = [BitConverter]::ToUInt32($out,0); $ph = [BitConverter]::ToUInt32($out,4)
    $d4 = [BitConverter]::ToUInt32($out,80); $d5 = [BitConverter]::ToUInt32($out,84)
    $d6 = [BitConverter]::ToUInt32($out,88); $d7 = [BitConverter]::ToUInt32($out,92)
    $hex = if ($size -ge 364) { ($out[112..363] | ForEach-Object { $_.ToString("X2") }) -join '' } else { "" }
    "ok=$ok err=$err sz=0x{0:X} ph={1} d4=0x{2:X} d5=0x{3:X} d6={4} d7=0x{5:X} cfg=$hex" -f $sz,$ph,$d4,$d5,$d6,$d7
  }
  Write-Host ("A warm 364B : " + (T 0 0 364))
  $dev = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
  $full = $dev.InstanceId
  $wd = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
    "-NoProfile","-Command","Start-Sleep -Seconds 90;",
    "pnputil /scan-devices 2>`$null | Out-Null; pnputil /enable-device '$full' 2>`$null | Out-Null; exit")
  Write-Host "displace: remove-device"
  pnputil /remove-device $full | Out-Null
  Write-Host ("B disp dump : " + (T 0 0 364))
  Write-Host ("C disp CLS  : " + (T 0 2 108))
  Write-Host ("EntryStage2 : " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\GuRtUnlock\Parameters' -ErrorAction SilentlyContinue).EntryStage)
  pnputil /scan-devices | Out-Null
  $bound = $false
  for ($j = 0; $j -lt 30; $j++) {
    Start-Sleep 1
    $d = Get-PnpDevice | Where-Object { $_.InstanceId -like "$id*" -and $_.Class -eq 'Display' } | Select-Object -First 1
    if ($d -and $d.Status -eq 'OK' -and $d.Present) { $bound = $true; break }
  }
  Write-Host "handover: bound=$bound after $($j+1)s"
  Start-Sleep 8
  if (-not $wd.HasExited) { Stop-Process $wd -Force }
  Set-Content (Join-Path $build 'verdict4-DONE') "done"
} finally { Stop-Transcript }
