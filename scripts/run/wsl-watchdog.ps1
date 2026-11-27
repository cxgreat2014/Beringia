# SPDX-License-Identifier: GPL-2.0-only
# wsl-watchdog.ps1 - Beringia WSL watchdog, v2 (Windows PowerShell 5.1+).
#
# Why: a hosted-XNU experiment can wedge the whole WSL2 VM (a CPU stuck in the foreign world with interrupts off
# eventually takes the rest of Linux down). When that happens the evidence has to be kept and the VM restarted, even
# when nobody is watching.
#
# How:
#   * It does NOTHING unless a run is armed. scripts/run/boot.sh writes <StateDir>\run\armed (key=value lines: run_id,
#     run_dir, distro, expires = unix seconds, stop_script) right before it loads the modules; scripts/run/stop.sh
#     removes it; an armed run also expires on its own (BERINGIA_WATCH_MINUTES), and then the watchdog runs its
#     stop_script (an orderly unload) before disarming, because once the probes stop WSL idles the VM out and would
#     kill a still-running XNU. While nothing is armed the watchdog never runs
#     wsl.exe, so it neither keeps the VM alive nor starts it again after a crash (which would overwrite the console
#     log of the crash).
#   * While a run is armed, every PollSeconds it
#       - mirrors the host-side kernel console log (.wslconfig: wsl2.debugConsoleLogFile) into the run directory,
#       - checks that the WSL VM is alive (its vmmemWSL process, same id as when the run was armed) and that the
#         distro answers (wsl -d <distro> -e true). Not `wsl --list --running`: WSL stops an idle distro after a few
#         seconds while the VM, and the hosted XNU in its kernel, keep running.
#   * Outcomes:
#       - VM process gone or replaced       -> incident "vm-exited" (usually a guest panic; panic=-1 resets the VM)
#       - no answer for StrikeLimit polls   -> incident "hang": evidence saved, then wsl --shutdown --force
#     An incident saves the console log, the watchdog log tail and a summary in the run directory, then disarms the
#     run (armed -> armed.<reason>), so the watchdog never acts twice on the same run.
#
# Logging: state changes and incidents only, to $LogFile.
# Testing: -DryRun (never shuts WSL down), -Once (a single poll), -ProbeCommand 'sleep 60' (simulate a hang).
# Install / update the scheduled task: powershell -File scripts/run/install-watchdog.ps1

param(
    [string]$StateDir = "$env:USERPROFILE\xnu-work",
    [string]$ConsoleLog = "$env:USERPROFILE\xnu-work\logs\wsl-console.log",
    [string]$Distro = 'Ubuntu-24.04',
    [int]$PollSeconds = 10,
    [int]$ProbeTimeoutSeconds = 20,
    [int]$StrikeLimit = 3,
    [int]$HeartbeatMinutes = 10,
    [string]$ProbeCommand = 'true',
    [string]$LogFile = '',
    [switch]$DryRun,
    [switch]$Once
)

$ErrorActionPreference = 'Continue'
if (-not $LogFile) { $LogFile = Join-Path $StateDir 'beringia-watchdog.log' }
$ArmFile = Join-Path $StateDir 'run\armed'

function Log([string]$msg) {
    $line = '{0}  {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    try { Add-Content -LiteralPath $LogFile -Value $line -ErrorAction Stop } catch {}
    Write-Host $line
}

# Runs wsl.exe with a hard timeout. Returns @{ TimedOut; ExitCode; Out }.
function Invoke-Wsl([string]$Arguments, [int]$TimeoutSec) {
    $r = @{ TimedOut = $false; ExitCode = -1; Out = '' }
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = 'wsl.exe'
        $psi.Arguments = $Arguments
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.EnvironmentVariables['WSL_UTF8'] = '1'
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $p = [System.Diagnostics.Process]::Start($psi)
        $out = $p.StandardOutput.ReadToEndAsync()
        $null = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            try { $p.Kill() } catch {}
            $r.TimedOut = $true
            return $r
        }
        $r.ExitCode = $p.ExitCode
        $r.Out = $out.Result
    } catch {
        $r.Out = $_.Exception.Message
    }
    return $r
}

# Copies a file that another process (the WSL service) keeps open for writing.
function Copy-Shared([string]$Src, [string]$Dst) {
    if (-not $Src -or -not (Test-Path -LiteralPath $Src)) { return $false }
    try {
        $in = [System.IO.File]::Open($Src, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]'ReadWrite, Delete')
        try {
            $out = [System.IO.File]::Open($Dst, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write,
                                          [System.IO.FileShare]::Read)
            try { $in.CopyTo($out) } finally { $out.Close() }
        } finally { $in.Close() }
        return $true
    } catch {
        return $false
    }
}

function Read-Arm {
    if (-not (Test-Path -LiteralPath $ArmFile)) { return $null }
    $h = @{}
    foreach ($line in @(Get-Content -LiteralPath $ArmFile -ErrorAction SilentlyContinue)) {
        $i = $line.IndexOf('=')
        if ($i -gt 0) { $h[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).Trim() }
    }
    if (-not $h['run_id']) { $h['run_id'] = 'unknown' }
    if (-not $h['distro']) { $h['distro'] = $Distro }
    return $h
}

function Get-RunDir($arm) {
    $d = $arm['run_dir']
    try { if ($d -and (Test-Path -LiteralPath $d -PathType Container)) { return $d } } catch {}
    $d = Join-Path $StateDir ('incidents\' + ($arm['run_id'] -replace '[^\w.-]', '_'))
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    return $d
}

function Disarm($arm, [string]$reason) {
    # Only this run's marker: boot.sh may have armed a new run in the meantime.
    $now = Read-Arm
    if (-not $now -or $now['run_id'] -ne $arm['run_id']) { return }
    $dest = Join-Path (Get-RunDir $arm) ('armed.' + $reason)
    try { Move-Item -LiteralPath $ArmFile -Destination $dest -Force -ErrorAction Stop }
    catch { Remove-Item -LiteralPath $ArmFile -Force -ErrorAction SilentlyContinue }
}

# The WSL VM's memory process. vmmemWSL on current WSL; plain vmmem (also used by other Hyper-V VMs such as Windows
# Sandbox or Docker) only on old builds that have no vmmemWSL.
function Get-VmIds {
    $vm = @(Get-Process -Name 'vmmemWSL' -ErrorAction SilentlyContinue)
    if (-not $vm) { $vm = @(Get-Process -Name 'vmmem' -ErrorAction SilentlyContinue) }
    return ($vm | ForEach-Object { $_.Id } | Sort-Object) -join ','
}

function Save-Incident($arm, [string]$reason, [string]$detail) {
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dir = Get-RunDir $arm
    $haveConsole = Copy-Shared $ConsoleLog (Join-Path $dir "console-$ts.log")
    $summary = @(
        "reason=$reason",
        "time=$(Get-Date -Format o)",
        "run_id=$($arm['run_id'])",
        "distro=$($arm['distro'])",
        "detail=$detail",
        "console_log_saved=$haveConsole",
        "dry_run=$DryRun"
    )
    Set-Content -LiteralPath (Join-Path $dir "incident-$ts.txt") -Value $summary
    if (Test-Path -LiteralPath $LogFile) {
        Get-Content -LiteralPath $LogFile -Tail 80 | Set-Content -LiteralPath (Join-Path $dir "watchdog-$ts.log")
    }
    return $dir
}

$mirrorStamp = ''
function Update-Mirror($arm) {
    if (-not (Test-Path -LiteralPath $ConsoleLog)) { return }
    $f = Get-Item -LiteralPath $ConsoleLog
    $stamp = '{0}/{1}' -f $f.Length, $f.LastWriteTimeUtc.Ticks
    if ($stamp -eq $script:mirrorStamp) { return }
    if (Copy-Shared $ConsoleLog (Join-Path (Get-RunDir $arm) 'console.log')) { $script:mirrorStamp = $stamp }
}

Log ("watchdog v2 started: state $StateDir, console log $ConsoleLog, poll ${PollSeconds}s, probe timeout " +
     "${ProbeTimeoutSeconds}s x $StrikeLimit strikes, dryRun=$DryRun")
if (-not (Test-Path -LiteralPath $ConsoleLog)) {
    Log "note: $ConsoleLog does not exist (yet); set wsl2.debugConsoleLogFile in .wslconfig to keep crash consoles"
}

$current = $null          # run_id of the armed run being watched
$vmId = ''                # vmmemWSL process id(s) when the run was first seen
$strikes = 0
$lastBeat = Get-Date

while ($true) {
    try {
        $arm = Read-Arm
        if (-not $arm) {
            if ($current) { Log "run ${current}: disarmed (marker removed)"; $current = $null }
        } else {
            if ($current -ne $arm['run_id']) {
                $current = $arm['run_id']
                $vmId = ''
                $strikes = 0
                $mirrorStamp = ''
                $lastBeat = Get-Date
                $exp = 'never'
                if ($arm['expires']) { $exp = [DateTimeOffset]::FromUnixTimeSeconds([int64]$arm['expires']).LocalDateTime }
                Log "run ${current}: armed (distro $($arm['distro']), dir $($arm['run_dir']), expires $exp)"
            }
            $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
            if ($arm['expires'] -and [int64]$arm['expires'] -lt $now) {
                Update-Mirror $arm
                # Stop the run instead of just walking away: without the probes WSL idles the VM out within a
                # minute or so, which would kill a still-running XNU with its disk image mounted read-write.
                if ($arm['stop_script'] -and (Get-VmIds)) {
                    if ($DryRun) {
                        Log "run ${current}: armed period expired; DRY RUN, not running $($arm['stop_script'])"
                    } else {
                        $st = Invoke-Wsl ('-d ' + $arm['distro'] + ' -u root -e ' + $arm['stop_script']) 180
                        $res = if ($st.TimedOut) { 'timed out' } else { "exit $($st.ExitCode)" }
                        Log "run ${current}: armed period expired; ran $($arm['stop_script']) ($res)"
                    }
                }
                Update-Mirror $arm
                Disarm $arm 'expired'
                Log "run ${current}: disarmed (expired)"
                $current = $null
            } else {
                Update-Mirror $arm
                $fail = $null
                # The VM, not the distro: WSL stops an idle distro after a few seconds while the VM (and the hosted
                # XNU in its kernel) keeps running. vmmemWSL is the VM's memory process; a new id means a new VM.
                $vmNow = Get-VmIds
                if (-not $vmId) { $vmId = $vmNow }
                if (-not $vmNow -or $vmNow -ne $vmId) {
                    $why = 'WSL VM process gone (guest panic/reset, or shut down by hand)'
                    if ($vmNow) { $why = "WSL VM restarted (process $vmId -> $vmNow)" }
                    $dir = Save-Incident $arm 'vm-exited' $why
                    Disarm $arm 'vm-exited'
                    Log "run ${current}: INCIDENT vm-exited ($why); evidence in $dir"
                    $current = $null
                } else {
                    # Starts the distro again if WSL stopped it for being idle; that is harmless (the VM is up).
                    $pr = Invoke-Wsl ('-d ' + $arm['distro'] + ' -e ' + $ProbeCommand) $ProbeTimeoutSeconds
                    if ($pr.TimedOut) { $fail = "probe timed out after ${ProbeTimeoutSeconds}s" }
                    elseif ($pr.ExitCode -ne 0) { $fail = "probe exited $($pr.ExitCode)" }
                }
                if ($current) {
                    if ($fail) {
                        $strikes++
                        Log "run ${current}: no answer ($fail), strike $strikes/$StrikeLimit"
                        if ($strikes -ge $StrikeLimit) {
                            $dir = Save-Incident $arm 'hang' $fail
                            if ($DryRun) {
                                Log "run ${current}: INCIDENT hang; DRY RUN, not shutting down; evidence in $dir"
                            } else {
                                Log "run ${current}: INCIDENT hang; evidence in $dir; running wsl --shutdown --force"
                                $sd = Invoke-Wsl '--shutdown --force' 120
                                Log ("run ${current}: shutdown " + $(if ($sd.TimedOut) { 'timed out' } else { "exit $($sd.ExitCode)" }))
                            }
                            Disarm $arm 'hang'
                            $current = $null
                        }
                    } else {
                        if ($strikes) { Log "run ${current}: answering again after $strikes strike(s)" }
                        $strikes = 0
                        if (((Get-Date) - $lastBeat).TotalMinutes -ge $HeartbeatMinutes) {
                            Log "run ${current}: still armed, WSL answering"
                            $lastBeat = Get-Date
                        }
                    }
                }
            }
        }
    } catch {
        Log "error: $($_.Exception.Message)"
    }
    if ($Once) { break }
    Start-Sleep -Seconds $PollSeconds
}
