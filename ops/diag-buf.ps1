$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'diag-buf.log') -Force
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
  function T([int]$pl,[int]$flags,[int]$size) {
    $in  = [BitConverter]::GetBytes([uint32]24) + [BitConverter]::GetBytes([uint32]1) +
           [BitConverter]::GetBytes([uint32]0) + [BitConverter]::GetBytes([uint32]0) +
           [BitConverter]::GetBytes([uint32]$pl) + [BitConverter]::GetBytes([uint32]$flags)
    $out = New-Object byte[] $size; $ret = 0
    $h = [GuIo]::CreateFile("\.\GuRtUnlock", [uint32]3221225472, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h -eq -1 -or $h -eq [IntPtr]::Zero) { return "open-fail $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    $ok = [GuIo]::DeviceIoControl($h, 0x00222004, $in, $in.Length, $out, $out.Length, [ref]$ret, [IntPtr]::Zero)
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    [GuIo]::CloseHandle($h) | Out-Null
    $sz = [BitConverter]::ToUInt32($out,0); $ph = [BitConverter]::ToUInt32($out,4)
    $d4 = [BitConverter]::ToUInt32($out,80); $d5 = [BitConverter]::ToUInt32($out,84)
    $hex = if ($size -ge 364) { ($out[112..363] | ForEach-Object { $_.ToString("X2") }) -join '' } else { "" }
    "ok=$ok err=$err size=0x{0:X} phase={1} d4=0x{2:X} d5=0x{3:X} d6={4} d7=0x{5:X} cfg=$hex" -f $sz,$ph,$d4,$d5,[BitConverter]::ToUInt32($out,88),[BitConverter]::ToUInt32($out,92)
  }
  Write-Host ("108B PL0 F0 : " + (T 0 0 108))
  Write-Host ("364B PL0 F0 : " + (T 0 0 364))
  Write-Host ("364B PL0 F0b: " + (T 0 0 364))
} finally { Stop-Transcript }
