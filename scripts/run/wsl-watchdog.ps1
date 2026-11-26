# beringia-watchdog.ps1
# Watchdog for WSL kernel-module bring-up: when a Claude Code session stalls (a WSL hang can block the agent),
# force-restart WSL so the agent unblocks.
#
# Trigger: the newest Claude Code session .jsonl has had no writes for $IdleMinutes (default 10).
# Action:  wsl.exe --shutdown --force  (kills ALL WSL instances; unsaved WSL work is lost — by design here).
#
# Run as a scheduled task at logon, or start manually:  powershell -File beringia-watchdog.ps1

param(
    [int]$IdleMinutes = 10,
    [int]$PollSeconds = 30,
    [string]$ProjectsDir = "$env:USERPROFILE\.claude\projects",
    [string]$LogFile = "$env:USERPROFILE\xnu-work\beringia-watchdog.log"
)

$ErrorActionPreference = 'Continue'
$lastFire = [DateTime]::MinValue

function Log($msg) {
    $line = "{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    try { Add-Content -Path $LogFile -Value $line -ErrorAction Stop } catch {}
    Write-Host $line
}

Log "watchdog started (idle>${IdleMinutes}min, poll ${PollSeconds}s, dir=$ProjectsDir)"

while ($true) {
    try {
        # Newest session transcript across all project folders (the active one).
        $newest = Get-ChildItem -Path $ProjectsDir -Filter *.jsonl -Recurse -File -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime -Descending | Select-Object -First 1

        if ($newest) {
            $idle = (Get-Date) - $newest.LastWriteTime
            if ($idle.TotalMinutes -ge $IdleMinutes) {
                # Don't fire more than once per inactivity period (WSL needs time to come back + agent to resume).
                if (((Get-Date) - $lastFire).TotalMinutes -lt 2) {
                    # recently fired; skip
                } else {
                    $lastFire = Get-Date
                    Log ("INACTIVE {0:N1} min on {1}; running wsl --shutdown --force" -f $idle.TotalMinutes, $newest.FullName)
                    & wsl.exe --shutdown --force 2>&1 | Out-String | ForEach-Object { Log $_ }
                    Log "shutdown issued"
                }
            }
        }
    } catch {
        Log "error: $($_.Exception.Message)"
    }
    Start-Sleep -Seconds $PollSeconds
}
