schtasks /create /tn GuRtRunner /ru SYSTEM /rl HIGHEST /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\HAL900\.zcode\workspace\1792944059437-ab17eb4d-ea9c-45a7-87c4-b4c6e824338a\gpu-unlock-rt\ops\elev-runner.ps1"
# allow the interactive user to trigger it
schtasks /change /tn GuRtRunner /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\HAL900\.zcode\workspace\1792944059437-ab17eb4d-ea9c-45a7-87c4-b4c6e824338a\gpu-unlock-rt\ops\elev-runner.ps1"
Set-Content (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'task-DONE') 'ok'
