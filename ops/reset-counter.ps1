# Reset the crash counter (benignly tripped by verification boots) and arm
# the filter for the BOOT-TIME transaction: the filter sees the boot start
# (StartSeen=1 proven), holds it, transacts in the exact Linux window.
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'
Set-ItemProperty $p -Name BootCount -Value 0 -Type DWord
Remove-ItemProperty $p -Name Passive -ErrorAction SilentlyContinue
Set-ItemProperty $p -Name Enable -Value 1 -Type DWord
Set-ItemProperty $p -Name LastRunState -Value 0 -Type DWord
$props = Get-ItemProperty $p
$out = "BootCount=" + $props.BootCount + " Enable=" + $props.Enable + " Passive=[" + $props.Passive + "]"
$out
Set-Content 'C:\Users\HAL900\.zcode\workspace\1792944059437-ab17eb4d-ea9c-45a7-87c4-b4c6e824338a\gpu-unlock-rt\build\armed.txt' $out
"ARMED FOR BOOT: reboot now; expect longer early-boot darkness (the held start);"
"recovery kit: docs\RECOVERY-KIT.md (counter re-arms itself: 3 bad loads -> passive)"
