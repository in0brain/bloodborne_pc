#Requires -Version 5.1
<#
.SYNOPSIS
    Deploys bbport on Windows without questions, then checks that the game starts.

.DESCRIPTION
    The steps of setup.bat's Install / Update (tools\setup\BbportSetup.cs) as a script: for a new
    machine, a second copy beside a working one, or a test of what the repository publishes.

      1. Checks: 64-bit Windows 10 1803 or newer, the GPU driver's Vulkan loader, the free space,
         the game folder (eboot.bin, sce_module, version 01.09 in sce_sys\param.sfo).
      2. MSYS2 at -Msys2Dir. When that folder does not exist, MSYS2's portable archive is
         extracted there: no installer, no administrator rights, nothing outside the folder.
      3. The MSYS2 packages README "Windows" lists. A new MSYS2 is updated first. An existing one
         is updated only when packages are missing, vulkan-headers is older than 1.4.350, or with
         -UpdateMsys2: an update can replace the compiler that other builds there use.
      4. The sources: -Repo at -Branch with its submodules, cloned into -InstallDir (new, empty,
         or holding only the game dump), or the checkout there fast-forwarded (not with -NoPull,
         local changes or another branch checked out).
      5. -Fsr4Models, -DlssModel: the optional upscaler downloads.
      6. The build: build.sh in MSYS2's CLANG64 environment (out\bb-probe.exe).
      7. out\game_dir.txt, and Bloodborne.cmd when there is none (setup.bat writes a fuller one).
         -Settings FILE: its key=value lines go into bbport.ini (a key there is replaced, the
         other settings stay), e.g. configs\keymap-in0brain.ini, a keyboard and mouse layout.
      8. Checks without a game window: bb-probe.exe --vulkan-only (its DLLs load and a Vulkan
         device runs a submission) and the launcher's dry run (BB_DRY_RUN=1: the game image and
         the patches are prepared, the game is not started).
      9. -SmokeTest: starts the game, waits until it has rendered for 30 s (the logos and the
         title screen; "Frame stats" lines in logs\bbport-*.log), then closes it. Leave the
         window open meanwhile.

    Everything printed also goes to <InstallDir>\logs\deploy-<time>.log. Run it again to update.
    git and pacman in MSYS2 take a proxy from HTTPS_PROXY / HTTP_PROXY, not from Windows.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\deploy_windows.ps1 -InstallDir D:\Games\bbport -GameDir D:\Games\CUSA03173 -SmokeTest

.EXAMPLE
    A copy with its own MSYS2, which tests the dependencies from nothing and leaves C:\msys64 alone:

    powershell -NoProfile -ExecutionPolicy Bypass -File deploy_windows.ps1 -InstallDir E:\bbtest\bbport -Msys2Dir E:\bbtest\msys64 -GameDir D:\Games\CUSA03173 -SmokeTest

.EXAMPLE
    The repository's keyboard and mouse layout in a copy's settings (the path is found in the
    install folder once the sources are there):

    powershell -NoProfile -ExecutionPolicy Bypass -File tools\deploy_windows.ps1 -InstallDir D:\Games\bbport -GameDir D:\Games\CUSA03173 -Settings configs\keymap-in0brain.ini
#>
[CmdletBinding()]
param(
    # The folder for the sources, the build, saves (user\), settings (bbport.ini) and logs.
    [Parameter(Mandatory = $true)][string]$InstallDir,
    # The game dump (eboot.bin, sce_module, sce_sys, dvdroot_ps4). Default: the one remembered in
    # <InstallDir>\out\game_dir.txt.
    [string]$GameDir,
    # Default: BB_MSYS2, else C:\msys64.
    [string]$Msys2Dir,
    [string]$Repo = 'https://github.com/in0brain/bloodborne_pc.git',
    [string]$Branch = 'windows-port',
    [switch]$UpdateMsys2,
    [switch]$NoPull,
    [switch]$Fsr4Models,
    [switch]$DlssModel,
    # key=value lines for <InstallDir>\bbport.ini. A relative path is looked up from the current
    # folder, then from the install folder (configs\keymap-in0brain.ini of the sources).
    [string]$Settings,
    [switch]$SmokeTest,
    # How long the smoke test waits for frames (the first start compiles the shaders).
    [int]$SmokeSeconds = 300
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# README "Windows" and tools\setup\BbportSetup.cs list the same packages.
$Packages = @('git') + @('clang', 'lld', 'libc++', 'cmake', 'ninja', 'pkgconf', 'python', 'sdl3', 'boost',
    'fmt', 'glslang', 'spirv-cross', 'spirv-headers', 'vulkan-headers', 'vulkan-loader',
    'vulkan-memory-allocator', 'xxhash', 'zydis', 'robin-map', 'ffmpeg' | ForEach-Object { "mingw-w64-clang-x86_64-$_" })
# Older headers lack PhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE (README "Windows").
$VulkanHeadersMin = [version]'1.4.350'
$Msys2Archive = 'https://repo.msys2.org/distrib/msys2-x86_64-latest.sfx.exe'
# A game dump may share the install folder (the sources are then cloned around it).
$DumpEntries = @('eboot.bin', 'sce_module', 'sce_sys', 'dvdroot_ps4', 'sce_discmap.plt', 'sce_discmap_patch.plt')
# Free space, in GB: the sources and the build (~1 GB) with room for shader caches and logs;
# MSYS2 with the packages and pacman's download cache.
$InstallSpaceGB = 4
$Msys2SpaceGB = 6

$script:Log = $null
$script:StepNumber = 0

# ---- output ------------------------------------------------------------------------------------

function Write-Log([string]$Text = '', [string]$Color) {
    if ($Color) { Write-Host $Text -ForegroundColor $Color } else { Write-Host $Text }
    if ($script:Log) { $script:Log.WriteLine($Text) }
}

function Write-Step([string]$Text) {
    $script:StepNumber++
    Write-Log
    Write-Log "== $script:StepNumber. $Text" Cyan
}

function Fail([string]$Text) { throw $Text }

function Open-Log {
    $logs = Join-Path $InstallDir 'logs'
    New-Item -ItemType Directory -Force $logs | Out-Null
    $path = Join-Path $logs ('deploy-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
    $script:Log = New-Object IO.StreamWriter($path, $true, (New-Object Text.UTF8Encoding($false)))
    $script:Log.AutoFlush = $true
    $path
}

# ---- child processes ---------------------------------------------------------------------------

# Exit codes Windows gives a program that could not start or crashed, as NTSTATUS names.
function Format-ExitCode([int]$Code) {
    $names = @{}
    $names[-1073741515] = 'a DLL it needs was not found (STATUS_DLL_NOT_FOUND)'
    $names[-1073741511] = 'a DLL lacks a function it needs (STATUS_ENTRYPOINT_NOT_FOUND)'
    $names[-1073741701] = 'a DLL of the wrong architecture (STATUS_INVALID_IMAGE_FORMAT)'
    $names[-1073741819] = 'crash (STATUS_ACCESS_VIOLATION)'
    $names[-1073741795] = 'illegal instruction (STATUS_ILLEGAL_INSTRUCTION)'
    if ($names.ContainsKey($Code)) { return ('0x{0:X8}: {1}' -f $Code, $names[$Code]) }
    if ($Code -lt 0) { return ('0x{0:X8}' -f $Code) }
    [string]$Code
}

# Runs a program; its standard output goes to the console and the log line by line (callers
# merge standard error into it). Returns the exit code, or with -Capture the lines unlogged.
function Invoke-Child {
    param([string]$File, [string]$Arguments, [string]$Directory, [hashtable]$Environment = @{}, [switch]$Capture)
    $info = New-Object Diagnostics.ProcessStartInfo $File, $Arguments
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.StandardOutputEncoding = New-Object Text.UTF8Encoding($false)
    if ($Directory) { $info.WorkingDirectory = $Directory }
    foreach ($key in $Environment.Keys) { $info.EnvironmentVariables[$key] = $Environment[$key] }
    $process = [Diagnostics.Process]::Start($info)
    $lines = New-Object Collections.Generic.List[string]
    while ($null -ne ($line = $process.StandardOutput.ReadLine())) {
        if ($Capture) { $lines.Add($line) } else { Write-Log $line }
    }
    $process.WaitForExit()
    if ($Capture) { return [pscustomobject]@{ ExitCode = $process.ExitCode; Output = $lines.ToArray() } }
    $process.ExitCode
}

function Assert-Exit([int]$Code, [string]$What) {
    if ($Code -ne 0) { Fail "$What failed (exit code $(Format-ExitCode $Code))" }
}

function ConvertTo-MsysPath([string]$Path) {
    if ($Path -notmatch '^([A-Za-z]):\\?(.*)$') { Fail "Not a path on a drive: $Path" }
    '/' + $Matches[1].ToLower() + '/' + ($Matches[2] -replace '\\', '/')
}

# A command in a CLANG64 login shell (its clang, cmake, ninja, pkg-config, python, git),
# optionally in a folder. Commands quote with single quotes: the whole command is one
# double-quoted argument of bash.exe.
function Invoke-Bash {
    param([string]$Command, [string]$Directory, [switch]$Capture)
    if ($Command.Contains('"')) { Fail "internal: double quote in a bash command: $Command" }
    $script = 'exec 2>&1; '
    if ($Directory) { $script += "cd '" + (ConvertTo-MsysPath $Directory) + "' && " }
    if (-not $Capture) { Write-Log "`$ $Command" DarkGray }
    Invoke-Child (Join-Path $Msys2Dir 'usr\bin\bash.exe') ('-lc "' + $script + $Command + '"') `
        -Environment @{ MSYSTEM = 'CLANG64'; CHERE_INVOKING = '1' } -Capture:$Capture
}

# A batch file through cmd.exe, standard error merged.
function Invoke-Cmd([string]$Command, [hashtable]$Environment = @{}) {
    Write-Log "> $Command" DarkGray
    Invoke-Child $env:ComSpec ('/d /s /c "' + $Command + ' 2>&1"') -Directory $InstallDir -Environment $Environment
}

# ---- checks ------------------------------------------------------------------------------------

function Resolve-FullPath([string]$Path) {
    [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)).TrimEnd('\')
}

# The build tools (and bash quoting here) handle spaces, quotes and non-ASCII names poorly.
function Assert-PlainPath([string]$Path, [string]$What) {
    if ($Path -match '[\s"''`$&%!^;]' -or $Path -match '[^\x21-\x7e]') {
        Fail "$What must be a path without spaces, quotes, special or non-ASCII characters: $Path"
    }
}

function Assert-FreeSpace([string]$Path, [double]$NeededGB, [string]$What) {
    $root = [IO.Path]::GetPathRoot($Path)
    $free = (New-Object IO.DriveInfo $root).AvailableFreeSpace / 1GB
    if ($free -lt $NeededGB) { Fail ('{0}: {1:N1} GB free on {2}, about {3} GB needed' -f $What, $free, $root, $NeededGB) }
    Write-Log ('{0}: {1:N0} GB free on {2}' -f $What, $free, $root)
}

# The PS4 param.sfo table: magic "\0PSF", key and data table offsets, 16-byte entries (key
# offset, format, length, capacity, data offset). Returns a hashtable, or $null.
function Read-ParamSfo([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $d = [IO.File]::ReadAllBytes($Path)
    if ($d.Length -lt 20 -or $d[0] -ne 0 -or [Text.Encoding]::ASCII.GetString($d, 1, 3) -ne 'PSF') { return $null }
    $keys = [BitConverter]::ToInt32($d, 8); $data = [BitConverter]::ToInt32($d, 12); $count = [BitConverter]::ToInt32($d, 16)
    $result = @{}
    for ($i = 0; $i -lt $count; $i++) {
        $e = 20 + 16 * $i
        $k = $keys + [BitConverter]::ToUInt16($d, $e); $format = [BitConverter]::ToUInt16($d, $e + 2)
        $length = [BitConverter]::ToInt32($d, $e + 4); $offset = $data + [BitConverter]::ToInt32($d, $e + 12)
        $end = $k
        while ($end -lt $d.Length -and $d[$end] -ne 0) { $end++ }
        $key = [Text.Encoding]::ASCII.GetString($d, $k, $end - $k)
        if ($format -eq 0x0204 -or $format -eq 0x0004) {
            $result[$key] = [Text.Encoding]::UTF8.GetString($d, $offset, $length).TrimEnd([char]0)
        } elseif ($format -eq 0x0404) {
            $result[$key] = [string][BitConverter]::ToUInt32($d, $offset)
        }
    }
    $result
}

function Test-System {
    if (-not [Environment]::Is64BitOperatingSystem) { Fail 'bbport needs 64-bit Windows.' }
    $build = [Environment]::OSVersion.Version.Build
    if ($build -lt 17134) { Fail "bbport needs Windows 10 1803 (build 17134) or newer; this is build $build." }
    Write-Log "Windows build $build, 64-bit"
    if (-not (Test-Path (Join-Path $env:WINDIR 'System32\vulkan-1.dll'))) {
        Fail 'No Vulkan loader (System32\vulkan-1.dll): install or update the GPU driver.'
    }
    Write-Log 'Vulkan loader: System32\vulkan-1.dll'
    if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue) -and -not (Test-Path (Join-Path $Msys2Dir 'usr\bin\bash.exe'))) {
        Fail 'curl.exe (part of Windows 10 1803+) is missing, and MSYS2 has to be downloaded.'
    }
}

function Test-GameDir {
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'eboot.bin'))) { Fail "No eboot.bin in $GameDir (the folder of your Bloodborne dump)." }
    if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'sce_module'))) { Fail "No sce_module folder in $GameDir." }
    $sfo = Read-ParamSfo (Join-Path $GameDir 'sce_sys\param.sfo')
    if (-not $sfo) {
        Write-Log "sce_sys\param.sfo is missing or unreadable: the game version is unknown (v01.09 is needed)." Yellow
        return
    }
    $version = if ($sfo.ContainsKey('APP_VER')) { $sfo['APP_VER'] } else { '?' }
    $title = if ($sfo.ContainsKey('TITLE')) { $sfo['TITLE'] } else { '?' }
    $id = if ($sfo.ContainsKey('TITLE_ID')) { $sfo['TITLE_ID'] } else { '?' }
    Write-Log "Game: $title $id, version $version"
    if (-not $version.StartsWith('01.09')) { Fail "bbport needs game version 01.09; $GameDir has $version (install the 1.09 update into the dump)." }
}

# ---- MSYS2 -------------------------------------------------------------------------------------

function Install-Msys2 {
    $parent = Split-Path $Msys2Dir -Parent
    New-Item -ItemType Directory -Force $parent | Out-Null
    $download = Join-Path $env:TEMP "bbport-msys2-$PID.sfx.exe"
    # Extracted beside the target (same drive, so the move below is a rename): the archive
    # always unpacks into a folder named msys64.
    $stage = Join-Path $parent ".bbport-msys2-$PID"
    try {
        Write-Log "Downloading $Msys2Archive"
        & curl.exe -fsSL --retry 3 -o $download $Msys2Archive
        Assert-Exit $LASTEXITCODE 'Downloading MSYS2'
        Write-Log ('{0:N0} MB' -f ((Get-Item $download).Length / 1MB))
        Write-Log "Extracting MSYS2 to $Msys2Dir"
        Assert-Exit (Invoke-Child $download "-y -o$stage" -Capture).ExitCode 'Extracting MSYS2'
        Move-Item (Join-Path $stage 'msys64') $Msys2Dir
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue $download
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $stage
    }
    if (-not (Test-Path (Join-Path $Msys2Dir 'usr\bin\bash.exe'))) { Fail "MSYS2 did not extract to $Msys2Dir" }
    # The first login shell creates the home folder and pacman's keyring.
    Write-Log 'First start of MSYS2 (pacman keyring)'
    Assert-Exit (Invoke-Bash 'true') 'The first start of MSYS2'
}

function Get-MissingPackages {
    # pacman -T prints the names that are not installed.
    @((Invoke-Bash ('pacman -T ' + ($Packages -join ' ')) -Capture).Output | Where-Object { $_ })
}

function Get-VulkanHeadersVersion {
    $result = Invoke-Bash 'pacman -Q mingw-w64-clang-x86_64-vulkan-headers' -Capture
    # "mingw-w64-clang-x86_64-vulkan-headers 1~1.4.363.0-1": MSYS2 writes the epoch with a tilde.
    if ($result.ExitCode -eq 0 -and ($result.Output -join ' ') -match ' (?:\d+[~:])?(\d+\.\d+\.\d+)') { return [version]$Matches[1] }
    $null
}

function Update-Packages([bool]$Fresh) {
    $missing = @(Get-MissingPackages)
    $headers = Get-VulkanHeadersVersion
    $reasons = @()
    if ($Fresh) { $reasons += 'new MSYS2' }
    if ($UpdateMsys2) { $reasons += '-UpdateMsys2' }
    if ($missing.Count) { $reasons += "missing: $($missing -join ', ')" }
    if ($headers -and $headers -lt $VulkanHeadersMin) { $reasons += "vulkan-headers $headers < $VulkanHeadersMin" }
    if (-not $reasons) {
        Write-Log "All $($Packages.Count) packages are installed (vulkan-headers $headers); no update (-UpdateMsys2 forces one)."
        return
    }
    Write-Log ('Updating MSYS2 and installing the packages: ' + ($reasons -join '; '))
    # As MSYS2's documentation does: a stale package database installs too old packages. The
    # first update may only replace MSYS2's own core and end early; the second does the rest.
    Invoke-Bash 'pacman -Syuu --noconfirm' | Out-Null
    Assert-Exit (Invoke-Bash 'pacman -Syuu --noconfirm') 'Updating MSYS2'
    Assert-Exit (Invoke-Bash ('pacman -S --needed --noconfirm ' + ($Packages -join ' '))) 'Installing the MSYS2 packages'
    $missing = @(Get-MissingPackages)
    if ($missing.Count) { Fail "Still missing after the install: $($missing -join ', ')" }
    $headers = Get-VulkanHeadersVersion
    if (-not $headers -or $headers -lt $VulkanHeadersMin) { Fail "vulkan-headers $headers is older than $VulkanHeadersMin after the update." }
    Write-Log "Packages ready (vulkan-headers $headers)."
}

# ---- sources -----------------------------------------------------------------------------------

function Get-GitLine([string]$Arguments) {
    $result = Invoke-Bash "git -c safe.directory='*' $Arguments" -Directory $InstallDir -Capture
    (@($result.Output) -join "`n").Trim()
}

function Sync-Sources {
    $git = "git -c safe.directory='*'"
    if (Test-Path (Join-Path $InstallDir '.git')) {
        Write-Log "Checkout in $InstallDir"
        $current = Get-GitLine 'rev-parse --abbrev-ref HEAD'
        $dirty = Get-GitLine 'status --porcelain --untracked-files=no --ignore-submodules=dirty'
        if ($NoPull) { Write-Log 'Not updated (-NoPull).' }
        elseif ($current -ne $Branch) { Write-Log "Branch $current is checked out, not ${Branch}: not updated." Yellow }
        elseif ($dirty) { Write-Log "Local changes: not updated.`n$dirty" Yellow }
        else { Assert-Exit (Invoke-Bash "$git pull --ff-only" -Directory $InstallDir) 'Updating the sources' }
    } elseif (Test-Path (Join-Path $InstallDir 'build.sh')) {
        Write-Log "Sources without git in ${InstallDir}: the submodules must be there already (a GitHub ZIP lacks them)." Yellow
        return
    } else {
        $other = @(Get-ChildItem -Force -LiteralPath $InstallDir | Where-Object { $_.Name -ne 'logs' -and $DumpEntries -notcontains $_.Name })
        if ($other.Count) { Fail "$InstallDir is not empty and has no bbport sources (it holds $($other[0].Name)); choose a new or empty folder." }
        # init + fetch instead of clone: the folder already holds this log, perhaps the dump.
        Write-Log "Cloning $Repo ($Branch) into $InstallDir"
        Assert-Exit (Invoke-Bash "$git init -q && $git remote add origin '$Repo' && $git fetch origin && $git checkout -q -b '$Branch' --track 'origin/$Branch'" -Directory $InstallDir) "Cloning $Repo"
        if ($InstallDir -eq $GameDir) {
            # The dump stays out of git status, as in a checkout that holds its game.
            $exclude = Join-Path $InstallDir '.git\info\exclude'
            $lines = "`n# Game data (Bloodborne v1.09 dump) kept next to the port`n" + (($DumpEntries | ForEach-Object { if ($_ -like '*.*') { "/$_" } else { "/$_/" } }) -join "`n") + "`n"
            [IO.File]::AppendAllText($exclude, $lines)
        }
    }
    Assert-Exit (Invoke-Bash "$git submodule update --init --recursive" -Directory $InstallDir) 'Fetching the submodules'
    Write-Log ('Sources: ' + (Get-GitLine "log -1 --format='%h %s'"))
}

# Without these the build fails later with a less helpful message.
function Test-Sources {
    $cmake = Join-Path $InstallDir 'gpu\CMakeLists.txt'
    if (-not (Test-Path (Join-Path $InstallDir 'run.bat')) -or -not (Test-Path (Join-Path $InstallDir 'scripts\run_game.py')) -or
        -not (Test-Path $cmake) -or -not (Select-String -Path $cmake -SimpleMatch 'if (WIN32)' -Quiet)) {
        Fail "These sources have no Windows support (branch $Branch of $Repo)."
    }
    foreach ($path in 'third_party\LibAtrac9\C\src', 'gpu\third_party\fsr-vulkan\src', 'gpu\third_party\imgui\imgui.h') {
        if (-not (Test-Path (Join-Path $InstallDir $path))) { Fail "Submodule missing: $path" }
    }
}

# ---- launcher and checks -----------------------------------------------------------------------

function Get-LaunchEnvironment {
    $environment = @{ PYTHONIOENCODING = 'utf-8' }
    if ($Msys2Dir -ne 'C:\msys64') { $environment['BB_MSYS2'] = $Msys2Dir }
    $environment
}

function Write-Launcher {
    $out = Join-Path $InstallDir 'out'
    New-Item -ItemType Directory -Force $out | Out-Null
    # No byte order mark: run_game.py reads the path as plain UTF-8.
    [IO.File]::WriteAllText((Join-Path $out 'game_dir.txt'), $GameDir)
    Write-Log "Game folder remembered in out\game_dir.txt: $GameDir"
    $launcher = Join-Path $InstallDir 'Bloodborne.cmd'
    if (Test-Path $launcher) { Write-Log 'Bloodborne.cmd exists: kept.'; return }
    $text = "@echo off`r`n" +
        "rem Bloodborne (bbport) launcher, written by tools\deploy_windows.ps1 (setup.bat writes one with more settings).`r`n" +
        "rem The settings are in bbport.ini (in-game menu: Insert / L3+R3).`r`n"
    if ($Msys2Dir -ne 'C:\msys64') { $text += "set `"BB_MSYS2=$Msys2Dir`"`r`n" }
    $text += "call `"%~dp0run.bat`" %*`r`nif errorlevel 1 pause`r`n"
    [IO.File]::WriteAllText($launcher, $text, [Text.Encoding]::ASCII)
    Write-Log "Wrote $launcher"
}

function Resolve-Settings {
    if ([IO.Path]::IsPathRooted($Settings)) { return $Settings }
    foreach ($base in (Get-Location).ProviderPath, $InstallDir) {
        $path = Join-Path $base $Settings
        if (Test-Path -LiteralPath $path -PathType Leaf) { return $path }
    }
    Fail "-Settings: $Settings is neither in $((Get-Location).ProviderPath) nor in $InstallDir"
}

# -Settings into bbport.ini as setup.bat's program writes it: a key already there is replaced in
# place, the others are appended. The game reads the defaults first, so a short file is enough.
function Merge-Settings {
    $source = Resolve-Settings
    $values = [ordered]@{}
    foreach ($line in Get-Content -LiteralPath $source) {
        if ($line -match '^\s*([A-Za-z0-9_]+)\s*=(.*)$') { $values[$Matches[1]] = $Matches[2].Trim() }
    }
    if (-not $values.Count) { Fail "-Settings: no key=value lines in $source" }
    $ini = Join-Path $InstallDir 'bbport.ini'
    $lines = New-Object Collections.Generic.List[string]
    if (Test-Path $ini) { foreach ($line in @(Get-Content -LiteralPath $ini)) { $lines.Add($line) } }
    if (-not $lines.Count) { $lines.Add('# bbport settings (in-game menu: Insert / L3+R3)') }
    $replaced = @{}
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*([A-Za-z0-9_]+)\s*=' -and $values.Contains($Matches[1])) {
            $key = $Matches[1]
            $lines[$i] = "$key=$($values[$key])"
            $replaced[$key] = $true
        }
    }
    foreach ($key in $values.Keys) { if (-not $replaced.ContainsKey($key)) { $lines.Add("$key=$($values[$key])") } }
    # CRLF, as the game writes the file on Windows.
    [IO.File]::WriteAllText($ini, (($lines -join "`r`n") + "`r`n"))
    Write-Log "Settings from ${source}: $($values.Count) values in bbport.ini ($($replaced.Count) replaced)"
}

function Test-Vulkan {
    $probe = Join-Path $InstallDir 'out\bb-probe.exe'
    if (-not (Test-Path $probe)) { Fail "The build left no $probe" }
    # MSYS2's DLLs (libc++, SDL3, FFmpeg, ...) as run_game.py puts them on PATH; System32 is searched
    # first, so the Vulkan loader is the driver's.
    $environment = @{ PATH = (Join-Path $Msys2Dir 'clang64\bin') + ';' + $env:PATH }
    $code = Invoke-Cmd "`"$probe`" --vulkan-only" $environment
    if ($code -ne 0) {
        Write-Log 'DLLs bb-probe.exe does not find:' Yellow
        Invoke-Bash "ldd out/bb-probe.exe | grep -i 'not found' || echo '(none)'" -Directory $InstallDir | Out-Null
    }
    Assert-Exit $code 'bb-probe.exe --vulkan-only'
    $caps = Join-Path $InstallDir 'out\bb-gpu-capabilities.exe'
    if (Test-Path $caps) { Invoke-Cmd "`"$caps`"" $environment | Out-Null }
}

function Test-DryRun {
    $environment = Get-LaunchEnvironment
    $environment['BB_DRY_RUN'] = '1'
    Assert-Exit (Invoke-Cmd "`"$(Join-Path $InstallDir 'run.bat')`" --game-dir `"$GameDir`"" $environment) 'The launcher dry run (run.bat)'
}

function Read-Shared([string]$Path) {
    # The launcher keeps the log open for writing.
    $stream = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try { (New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $stream.Dispose() }
}

function Stop-Game([Diagnostics.Process]$Launcher) {
    $probe = Join-Path $InstallDir 'out\bb-probe.exe'
    # Only this copy's game: another bbport may be running from another folder.
    Get-Process bb-probe -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $probe } | Stop-Process -Force
    if (-not $Launcher.WaitForExit(20000)) { & taskkill.exe /T /F /PID $Launcher.Id | Out-Null }
}

function Test-Game {
    $logs = Join-Path $InstallDir 'logs'
    $output = Join-Path $logs ('smoke-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
    $environment = Get-LaunchEnvironment
    $environment['BB_FRAME_STATS'] = '1'
    $info = New-Object Diagnostics.ProcessStartInfo $env:ComSpec, ('/d /s /c ""' + (Join-Path $InstallDir 'run.bat') + '" --game-dir "' + $GameDir + '" > "' + $output + '" 2>&1"')
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $InstallDir
    foreach ($key in $environment.Keys) { $info.EnvironmentVariables[$key] = $environment[$key] }
    $started = Get-Date
    $launcher = [Diagnostics.Process]::Start($info)
    Write-Log "Game started: a window opens, leave it open; closed after 30 s of frames (at most $SmokeSeconds s). Launcher output: $output"
    # Frame reports come every 5 s. Six of them span the logos and the title screen ("press any
    # button", ~15 s after the start, ~14 draws/frame). Further on the game needs input: idle
    # at the title it plays its intro movie.
    $renderReports = 6
    $log = $null; $reports = @(); $passed = $false; $ended = $false
    try {
        while (((Get-Date) - $started).TotalSeconds -lt $SmokeSeconds) {
            Start-Sleep -Seconds 2
            if (-not $log) {
                $log = Get-ChildItem (Join-Path $logs 'bbport-*.log') -ErrorAction SilentlyContinue |
                    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1
                if ($log) { Write-Log "Game log: $($log.FullName)" }
            }
            if ($log) {
                $text = Read-Shared $log.FullName
                $now = @([regex]::Matches($text, 'Frame stats: ([0-9.]+) FPS.*?(\d+) draws/frame') |
                    ForEach-Object { [pscustomobject]@{ Fps = [double]$_.Groups[1].Value; Draws = [int]$_.Groups[2].Value } })
                for ($i = $reports.Count; $i -lt $now.Count; $i++) { Write-Log ('  {0} FPS, {1} draws/frame' -f $now[$i].Fps, $now[$i].Draws) }
                $reports = $now
                if (@($reports | Where-Object { $_.Fps -ge 1 }).Count -ge $renderReports) { $passed = $true; break }
            }
            if ($launcher.HasExited) { $ended = $true; break }
        }
    } finally {
        if (-not $launcher.HasExited) { Stop-Game $launcher }
    }
    if ($passed) {
        Write-Log ('Smoke test passed: the game rendered for {0} s, {1:N0} s after the start; closed it.' -f
            (5 * $renderReports), ((Get-Date) - $started).TotalSeconds) Green
        return
    }
    Write-Log 'Smoke test failed. The end of the launcher output:' Red
    if (Test-Path $output) { Get-Content $output -Tail 25 | ForEach-Object { Write-Log $_ } }
    if ($log) {
        Write-Log "The end of $($log.FullName):" Red
        (Read-Shared $log.FullName) -split "`r?`n" | Select-Object -Last 40 | ForEach-Object { Write-Log $_ }
    }
    # Exit code 0 is the game's own exit when its window is closed (gpu/shim/bbgpu.cpp).
    if ($ended -and $launcher.ExitCode -eq 0) { Fail 'The game window was closed during the test; run it again and leave the window open.' }
    if ($ended) { Fail "The game ended after $($reports.Count) frame reports (exit code $(Format-ExitCode $launcher.ExitCode))." }
    Fail "Fewer than $renderReports frame reports within $SmokeSeconds s (-SmokeSeconds waits longer)."
}

# ---- main --------------------------------------------------------------------------------------

$began = Get-Date
$InstallDir = Resolve-FullPath $InstallDir
if (-not $Msys2Dir) { $Msys2Dir = if ($env:BB_MSYS2) { $env:BB_MSYS2 } else { 'C:\msys64' } }
$Msys2Dir = Resolve-FullPath $Msys2Dir
if (-not $GameDir) {
    $remembered = Join-Path $InstallDir 'out\game_dir.txt'
    if (-not (Test-Path $remembered)) { Fail 'Pass -GameDir (the folder of your Bloodborne dump).' }
    $GameDir = (Get-Content -Raw $remembered).Trim()
}
$GameDir = Resolve-FullPath $GameDir
Assert-PlainPath $InstallDir '-InstallDir'
Assert-PlainPath $Msys2Dir '-Msys2Dir'
if ($GameDir.Contains('"')) { Fail "-GameDir must not contain double quotes: $GameDir" }
if ($Repo -match "['`"\s]" -or $Branch -match "['`"\s]") { Fail '-Repo and -Branch must not contain quotes or spaces.' }
# A wrong absolute path fails now, not after the build; a relative one may name a file of the
# sources, which a new install gets later.
if ($Settings -and [IO.Path]::IsPathRooted($Settings) -and -not (Test-Path -LiteralPath $Settings -PathType Leaf)) {
    Fail "-Settings: no file $Settings"
}

# Windows error boxes (a missing DLL, a crash) would wait for a click; the programs started here
# inherit this and fail with an exit code instead.
Add-Type -Namespace BbportDeploy -Name Native -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
$errorMode = [BbportDeploy.Native]::SetErrorMode(0x0003)  # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX

$status = 0
try {
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    $logPath = Open-Log
    Write-Log "bbport deployment $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Write-Log "Install: $InstallDir"
    Write-Log "Game:    $GameDir"
    Write-Log "MSYS2:   $Msys2Dir"
    Write-Log "Sources: $Repo ($Branch)"
    Write-Log "Log:     $logPath"

    Write-Step 'System and game folder'
    Test-System
    Test-GameDir
    $freshMsys2 = -not (Test-Path (Join-Path $Msys2Dir 'usr\bin\bash.exe'))
    Assert-FreeSpace $InstallDir $InstallSpaceGB 'Install folder'
    if ($freshMsys2) { Assert-FreeSpace $Msys2Dir $Msys2SpaceGB 'MSYS2' }

    Write-Step 'MSYS2'
    if ($freshMsys2) {
        if ((Test-Path $Msys2Dir) -and (Get-ChildItem -Force $Msys2Dir | Select-Object -First 1)) {
            Fail "$Msys2Dir exists but holds no MSYS2 (usr\bin\bash.exe): remove it or choose another -Msys2Dir."
        }
        Install-Msys2
    } else {
        Write-Log "MSYS2 found at $Msys2Dir"
    }

    Write-Step 'MSYS2 packages'
    Update-Packages $freshMsys2

    Write-Step 'Sources'
    Sync-Sources
    Test-Sources

    if ($Fsr4Models -or $DlssModel) {
        Write-Step 'Upscaler downloads'
        if ($Fsr4Models) { Assert-Exit (Invoke-Bash 'bash tools/fetch_fsr4_assets.sh' -Directory $InstallDir) 'Downloading the FSR 4 models' }
        if ($DlssModel) { Assert-Exit (Invoke-Bash 'bash tools/fetch_dlss.sh' -Directory $InstallDir) 'Downloading the DLSS model' }
    }

    Write-Step 'Build'
    Assert-Exit (Invoke-Bash 'bash build.sh' -Directory $InstallDir) 'The build (out\gpu-build.log has the GPU library log)'

    Write-Step 'Launcher and settings'
    Write-Launcher
    if ($Settings) { Merge-Settings }

    Write-Step 'Vulkan and DLLs (bb-probe.exe --vulkan-only)'
    Test-Vulkan

    Write-Step 'Launcher dry run (game image and patches)'
    Test-DryRun

    if ($SmokeTest) {
        Write-Step 'Smoke test: start the game'
        Test-Game
    }

    Write-Log
    Write-Log ('Done in {0:N0} min. Start the game with {1}' -f ((Get-Date) - $began).TotalMinutes, (Join-Path $InstallDir 'Bloodborne.cmd')) Green
} catch {
    Write-Log
    Write-Log "FAILED: $($_.Exception.Message)" Red
    if ($script:Log) { Write-Log "Log: $logPath" }
    $status = 1
} finally {
    [BbportDeploy.Native]::SetErrorMode($errorMode) | Out-Null
    if ($script:Log) { $script:Log.Dispose(); $script:Log = $null }
}
exit $status
