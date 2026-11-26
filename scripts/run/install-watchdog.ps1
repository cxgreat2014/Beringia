# SPDX-License-Identifier: GPL-2.0-only
# install-watchdog.ps1 - install (or update) and start the Beringia WSL watchdog scheduled task.
# Run as your normal user: powershell -File scripts/run/install-watchdog.ps1
# Copies the watchdog to a Windows-local path (so it runs while WSL is down), replaces any running instance and
# registers the task to start at logon. See scripts/run/wsl-watchdog.ps1 and docs/debugging.md.
$ErrorActionPreference = 'Stop'
$name = 'BeringiaWSLWatchdog'
$src = Join-Path $PSScriptRoot 'wsl-watchdog.ps1'
if (-not (Test-Path $src)) { throw "watchdog script not found: $src" }

$destDir = Join-Path $env:USERPROFILE 'xnu-work'
New-Item -ItemType Directory -Force -Path (Join-Path $destDir 'run'), (Join-Path $destDir 'logs') | Out-Null
$dest = Join-Path $destDir 'beringia-watchdog.ps1'

# Stop the old instance first: the task is single-instance, so a new start would otherwise be ignored.
if (Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
}
Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" |
    Where-Object { $_.CommandLine -like "*$dest*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Copy-Item -Path $src -Destination $dest -Force

$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$dest`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $name
Write-Host "$name registered and started (script: $dest)"
