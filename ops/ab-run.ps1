# Elevated A/B: same instance, PL=0/Flags=0 vs PL=3/Flags=1, direct prints.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'ab-run.log') -Force
try {
  Set-Location (Join-Path $root '..')

  function Invoke-Tower([int]$phaseLimit, [int]$flags) {
    $in  = [BitConverter]::GetBytes([uint32]24) + [BitConverter]::GetBytes([uint32]1) +
           [BitConverter]::GetBytes([uint32]0) + [BitConverter]::GetBytes([uint32]0) +
           [BitConverter]::GetBytes([uint32]$phaseLimit) + [BitConverter]::GetBytes([uint32]$flags)
    $out = New-Object byte[] 108
    $ret = 0
    $h = [GuIo]::CreateFile("\\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { return "open-failed $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $ok = [GuIo]::DeviceIoControl($h, 0x00222004, $in, $in.Length, $out, $out.Length, [ref]$ret, [IntPtr]::Zero)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [GuIo]::CloseHandle($h) | Out-Null
    $sz  = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 0))
    $ph  = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 4))
    $d0  = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 64))
    return "inLen=$($in.Length) ok=$ok err=$err size=$sz phase=$ph diag0=$d0"
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

  # device must be PnP-disabled for the tower, but the A/B is about IOCTL routing;
  # run both WITHOUT touching PnP so nothing else varies.
  Write-Host ("A PL=0/Flags=0 : " + (Invoke-Tower 0 0))
  Write-Host ("B PL=3/Flags=1 : " + (Invoke-Tower 3 1))
  Set-Content (Join-Path $build 'ab-run-DONE') 'ok'
} finally { Stop-Transcript }
