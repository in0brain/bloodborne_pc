# Windows: samples the running game's memory, video memory and GPU load into a CSV until it exits,
# to set beside the game log (logs\bbport-*.log; BB_FRAME_STATS=1 adds frame rate and stalls).
#   powershell -ExecutionPolicy Bypass -File tools\monitor_windows.ps1 [-Interval 2] [-Out file.csv]
# Columns: time, minutes since start, private and working set memory (MiB), dedicated and shared
# video memory (MiB, Windows' per-process GPU counters), 3D engine load (%).
param([double]$Interval = 2, [string]$Out = '', [int]$WaitSeconds = 120)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) {
    New-Item -ItemType Directory -Force (Join-Path $root 'logs') | Out-Null
    $Out = Join-Path $root ('logs\monitor-{0}.csv' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$deadline = (Get-Date).AddSeconds($WaitSeconds)
while (-not ($game = Get-Process bb-probe -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    if ((Get-Date) -gt $deadline) { Write-Host 'bb-probe is not running'; exit 1 }
    Start-Sleep -Seconds 1
}
$id = $game.Id
$counters = "\GPU Process Memory(pid_$id*)\Dedicated Usage", "\GPU Process Memory(pid_$id*)\Shared Usage",
            "\GPU Engine(pid_$id*engtype_3D)\Utilization Percentage"
'time,minutes,private_mib,working_set_mib,vram_dedicated_mib,vram_shared_mib,gpu_3d_percent' | Set-Content -Encoding ascii $Out
Write-Host "Monitoring bb-probe (pid $id) into $Out"
while ($true) {
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $p) { break }
    $dedicated = $shared = $load = 0.0
    try {
        foreach ($s in (Get-Counter -Counter $counters -SampleInterval 1 -MaxSamples 1 -ErrorAction SilentlyContinue).CounterSamples) {
            if ($s.Path -like '*dedicated usage') { $dedicated += $s.CookedValue }
            elseif ($s.Path -like '*shared usage') { $shared += $s.CookedValue }
            else { $load += $s.CookedValue }
        }
    } catch { }
    $line = '{0},{1:F2},{2:F0},{3:F0},{4:F0},{5:F0},{6:F1}' -f (Get-Date -Format 'HH:mm:ss'),
        ((Get-Date) - $p.StartTime).TotalMinutes, ($p.PrivateMemorySize64 / 1MB), ($p.WorkingSet64 / 1MB),
        ($dedicated / 1MB), ($shared / 1MB), $load
    Add-Content -Encoding ascii $Out $line
    Start-Sleep -Milliseconds ([math]::Max(0, ($Interval - 1) * 1000))
}
Write-Host "bb-probe exited; samples in $Out"
