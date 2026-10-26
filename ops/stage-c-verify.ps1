# Stage C verify (no elevation needed): read the filter's breadcrumb marks
# after the two inert reboots.
$p = 'HKLM:\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters'
$props = Get-ItemProperty $p -ErrorAction SilentlyContinue
"Attached  : $($props.Attached)"
"Bus       : $($props.Bus)"
"BootCount : $($props.BootCount)  (expect 2 after two inert boots; Passive NOT set)"
"Passive   : $($props.Passive)    (must be EMPTY)"
"Enable    : $($props.Enable)     (0 = pass-through, as installed)"
"EntryStage: $($props.EntryStage) (1 = DriverEntry completed)"
"LastRun*  : (must be empty - no transaction may have run)"
"svc       : $((Get-Service GuFltUnlock -ErrorAction SilentlyContinue).Status) (Stopped after boot is normal: demand-start, loads with the display stack)"
"GPU       : $((Get-PnpDevice -Class Display | Where-Object FriendlyName -match '3060').Status)"
