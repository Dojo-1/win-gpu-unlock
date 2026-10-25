# Elevated: (PL, Flags) isolation matrix against the live driver instance.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'matrix-run.log') -Force
try {
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
    $sz = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 0))
    $ph = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 4))
    $p1 = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 8))
    return "ok=$ok err=$err size=$sz phase=$ph st1=$p1"
  }
  foreach ($combo in @(@(0,0),@(0,1),@(1,0),@(1,1),@(2,1),@(3,0),@(3,1),@(12,1))) {
    Write-Host ("PL={0} F={1} : {2}" -f $combo[0], $combo[1], (Invoke-Tower $combo[0] $combo[1]))
  }
  Set-Content (Join-Path $build 'matrix-run-DONE') 'ok'
} finally { Stop-Transcript }
