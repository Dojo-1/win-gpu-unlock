# Disarm the filter to inert (Enable=0). The unlock is power-volatile (cleared
# at shutdown anyway); the filter stays attached, pass-through, harmless.
# Re-arm anytime with: ops\reset-counter.ps1
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'
Set-ItemProperty $p -Name Enable -Value 0 -Type DWord
$q = Get-ItemProperty $p
"Enable=" + $q.Enable + " BootCount=" + $q.BootCount + " -> INERT (pass-through; one command re-arms)"
