# Arm the filter permanently: every boot re-runs the unlock transaction.
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'
Set-ItemProperty $p -Name BootCount -Value 0 -Type DWord
Remove-ItemProperty $p -Name Passive -ErrorAction SilentlyContinue
Set-ItemProperty $p -Name Enable -Value 1 -Type DWord
$q = Get-ItemProperty $p
"ARMED: Enable=" + $q.Enable + " BootCount=" + $q.BootCount + " -> unlock re-applied at every boot"
