# One coherent transcript: stop, build, sign, start, test PL=2 inline.
$root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
Start-Transcript -Path (Join-Path $build 'one-shot.log') -Force
try {
  Set-Location (Join-Path $root '..')
  sc.exe stop GuRtUnlock | Out-Null
  for ($j = 0; $j -lt 20; $j++) { if ((Get-Service GuRtUnlock).Status -eq 'Stopped') { break }; Start-Sleep 2 }
  Write-Host "stop: $((Get-Service GuRtUnlock).Status)"

  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -NoSign
  if ($LASTEXITCODE) { Set-Content (Join-Path $build 'one-shot-FAILED') 'build'; return }
  Write-Host ("binary: {0} bytes at {1}" -f (Get-Item .\build\GuRtUnlock.sys).Length, (Get-Item .\build\GuRtUnlock.sys).LastWriteTime)

  & "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe" sign /fd SHA256 /f (Join-Path $build 'GuRtTest.pfx') /p gu-rt-test (Join-Path $build 'GuRtUnlock.sys') | Out-Null
  sc.exe start GuRtUnlock | Out-Null; Start-Sleep 2
  Write-Host "start: $((Get-Service GuRtUnlock).Status)"

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
    $d6 = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 88))
    $d7 = ('0x{0:X8}' -f [BitConverter]::ToUInt32($out, 92))
    return "ok=$ok err=$err size=$sz phase=$ph diag6=$d6 diag7=$d7"
  }
  # real flow: PnP-disable -> tower PL=3 -> enable, verdict in-band
  .\orchestrate.ps1 -PhaseLimit 3 -ConfirmBlackout
  Set-Content (Join-Path $build 'one-shot-DONE') 'ok'
} finally { Stop-Transcript }
