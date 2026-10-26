# Recovery kit — print this before Stage C

If the machine ever boot-loops or the display stack fails after the filter is
installed, follow top to bottom. Every step is verified-possible; none
requires reinstalling Windows.

## 0. First: the crash counter should have handled it

Generation 2 self-disarms after 3 bad loads (`BootCount >= 3` → permanent
pass-through). The expected worst case is: up to three crash-screens, then
normal boots forever. If you are reading this from another device because
even that did not hold, continue.

## 1. WinRE offline registry edit (under 5 minutes, fixes everything)

1. Force-off 2× to reach WinRE (or boot a Windows install USB → Repair).
2. Troubleshoot → Command Prompt.
3. Find the Windows drive letter (usually `D:` in WinRE):
   `dir D:\Windows\System32` (adjust until it lists).
4. `reg load HKLM\TEMPSYS D:\Windows\System32\config\SYSTEM`
5. Remove the filter registration (either step suffices; both is fine):
   - `reg delete "HKLM\TEMPSYS\ControlSet001\Services\GuFltUnlock" /f`
   - `reg delete "HKLM\TEMPSYS\ControlSet001\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" /v LowerFilters /f`
     (only GuFltUnlock was ever appended; deleting the whole value restores
     the pre-install state on this machine — it was empty)
6. `reg unload HKLM\TEMPSYS` → `exit` → reboot Windows normally.

## 2. If Windows boots but the display misbehaves

- `Win+Ctrl+Shift+B` restarts the display driver in-session.
- Admin PowerShell: `sc config GuFltUnlock start= disabled` then reboot.
- Or remove entirely:
  `sc delete GuFltUnlock` +
  `Remove-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -Name LowerFilters`

## 3. Manual crash-counter reset (after three failed-but-clean experiment boots)

Admin: `reg add HKLM\SYSTEM\CurrentControlSet\Services\GuFltUnlock\Parameters /v BootCount /t REG_DWORD /d 0 /f`

## 4. Last Known Good (if enabled before arming)

F8 at boot → Last known good configuration — boots the registry from before
the last successful boot, predating the filter arming.

## What can NEVER require an OS reinstall

The filter touches: one service key, one class-key value, one registry
parameters key, one .sys file. All four are deletable from WinRE in step 1.
There is no firmware, bootloader, or partition modification anywhere in this
project.
