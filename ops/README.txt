launch a step window (closes itself):
  powershell -NoProfile -ExecutionPolicy Bypass -File ops\step.ps1 -Step probe
(requires UAC approval; marker: build\step-<step>-DONE)
