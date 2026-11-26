# install-watchdog.ps1 — install and start the Beringia WSL watchdog scheduled task.
# Run once (as your normal user): powershell -File scripts/run/install-watchdog.ps1
# Copies the watchdog to a Windows-local path so it also runs while WSL is down, then registers the task.
$ErrorActionPreference = 'Stop'
$src = Join-Path $PSScriptRoot 'wsl-watchdog.ps1'
if (-not (Test-Path $src)) { throw "watchdog script not found: $src" }

$destDir = Join-Path $env:USERPROFILE 'xnu-work'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
$dest = Join-Path $destDir 'beringia-watchdog.ps1'
Copy-Item -Path $src -Destination $dest -Force

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$dest`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName 'BeringiaWSLWatchdog' -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'BeringiaWSLWatchdog'
Write-Host "BeringiaWSLWatchdog registered and started (script: $dest)"
