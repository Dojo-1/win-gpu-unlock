# SPDX-License-Identifier: GPL-2.0-only
# G1b (elevated): deploy GSP asset, self-sign the driver, enable test signing,
# register the service. Stops before destructive steps if Secure Boot is ON.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root '..\build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
Start-Transcript -Path (Join-Path $build 'g1b-transcript.log') -Force
try {
    # 0) Secure Boot gate: testsigning is impossible with SB on.
    $sb = $null
    try { $sb = Confirm-SecureBootUEFI } catch { $sb = "unknown ($($_.Exception.Message))" }
    Write-Host "SecureBoot: $sb"
    if ($sb -eq $true) {
        Write-Host "SECURE BOOT IS ON - disable it in BIOS setup first, then rerun this script."
        Set-Content (Join-Path $build 'g1b-NEEDS-SB-OFF') "secure boot on"
        return
    }

    # 1) deploy the 84 MB GSP image the driver reads at transaction time
    Copy-Item (Join-Path $root '..\..\gpu-unlock\firmware\fw\gsp_ga10x.bin') `
              "$env:SystemRoot\System32\drivers\GuRtGsp.bin" -Force
    $gsphash = (Get-FileHash "$env:SystemRoot\System32\drivers\GuRtGsp.bin" -Algorithm SHA256).Hash
    Write-Host "GuRtGsp.bin sha256: $gsphash"

    # 2) self-signed code-signing cert (machine store, exportable)
    $cert = Get-ChildItem Cert:\LocalMachine\My -CodeSigningCert |
            Where-Object Subject -eq 'CN=GuRtTest' | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Subject 'CN=GuRtTest' -Type CodeSigningCert `
                 -CertStoreLocation 'Cert:\LocalMachine\My' -NotAfter (Get-Date).AddYears(2)
        Write-Host "created cert thumbprint $($cert.Thumbprint)"
    }
    $cerFile = Join-Path $build 'GuRtTest.cer'
    Export-Certificate -Cert $cert -FilePath $cerFile | Out-Null
    foreach ($store in 'Root','TrustedPublisher') {
        $existing = Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object Thumbprint -eq $cert.Thumbprint
        if (-not $existing) { Import-Certificate -FilePath $cerFile -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null; Write-Host "imported to $store" }
    }
    $pfx = Join-Path $build 'GuRtTest.pfx'
    $pw = ConvertTo-SecureString 'gu-rt-test' -AsPlainText -Force
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null

    # 3) sign
    $signtool = "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
    & $signtool sign /fd SHA256 /f $pfx /p gu-rt-test (Join-Path $build 'GuRtUnlock.sys')
    if ($LASTEXITCODE) { Set-Content (Join-Path $build 'g1b-FAILED') "signtool $LASTEXITCODE"; return }
    & $signtool verify /v /pa (Join-Path $build 'GuRtUnlock.sys') | Select-Object -First 6

    # 4) test signing (no-op if already on)
    bcdedit /set testsigning on | Out-Null
    Write-Host "testsigning: set (effective after reboot)"

    # 5) register the service (start deferred until after reboot)
    if (-not (Get-Service GuRtUnlock -ErrorAction SilentlyContinue)) {
        sc.exe create GuRtUnlock type= kernel start= demand binPath= (Join-Path $build 'GuRtUnlock.sys')
    } else { Write-Host "service already present" }

    Set-Content (Join-Path $build 'g1b-DONE') "signed+testsigning+service; reboot required"
} finally {
    Stop-Transcript
}
