<#
  VozAlien installer for Windows (Steam).

    irm https://raw.githubusercontent.com/sidnei-almeida/alien-isolation-vozalien/main/install.ps1 | iex

  With options:
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/sidnei-almeida/alien-isolation-vozalien/main/install.ps1))) -GameDir 'D:\SteamLibrary\steamapps\common\Alien Isolation'
    & ([scriptblock]::Create((irm https://raw.githubusercontent.com/sidnei-almeida/alien-isolation-vozalien/main/install.ps1))) -Uninstall
#>
param(
    [string]$GameDir = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is very slow with the progress bar on PowerShell 5.1
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo        = 'sidnei-almeida/alien-isolation-vozalien'
$SteamSha    = 'DDE66521798AAE8C39ED4E7A3400F561FCECFFF83619D1426B9E0CEDF5819A90'
$UalUrl      = 'https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v10.0.1/Ultimate-ASI-Loader-NoPDB.zip'
$UalZipSha   = 'ACFEC08AABF361B412DB58726BDE20DF658B5430C043C241B6149C976CB63AB3'
$UalDllSha   = '8461BFD20D260087E76F9DBBAEFB0BC237C44A83505B5A88CC9CB61404023E85'
$LoaderNames = @('d3d11.dll', 'winmm.dll', 'dxgi.dll', 'version.dll', 'dinput8.dll', 'dsound.dll', 'winhttp.dll')

function Say([string]$m)  { Write-Host "==> $m" -ForegroundColor Green }
function Info([string]$m) { Write-Host "    $m" }
function Warn([string]$m) { Write-Host " !! $m" -ForegroundColor Yellow }
function Fail([string]$m) { Write-Host " xx $m" -ForegroundColor Red; throw $m }

function Get-Sha([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToUpperInvariant() }

function Test-Ual([string]$path) {
    try {
        $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
        return ($text.Contains('UltimateASILoader') -or $text.Contains('Ultimate-ASI-Loader'))
    } catch { return $false }
}

function Find-GameDir {
    $roots = New-Object System.Collections.Generic.List[string]
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        $p = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($p) {
            if ($p.SteamPath)   { $roots.Add(($p.SteamPath -replace '/', '\')) }
            if ($p.InstallPath) { $roots.Add($p.InstallPath) }
        }
    }
    if (${env:ProgramFiles(x86)}) { $roots.Add((Join-Path ${env:ProgramFiles(x86)} 'Steam')) }

    $libs = New-Object System.Collections.Generic.List[string]
    foreach ($r in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $r)) { continue }
        $libs.Add($r)
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($line in (Get-Content -LiteralPath $vdf)) {
                if ($line -match '^\s*"path"\s+"(.+)"\s*$') { $libs.Add(($Matches[1] -replace '\\\\', '\')) }
            }
        }
    }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $d = Join-Path $l 'steamapps\common\Alien Isolation'
        if (Test-Path -LiteralPath (Join-Path $d 'AI.exe')) { return $d }
    }
    return $null
}

function Copy-Safe([string]$from, [string]$to) {
    try { Copy-Item -LiteralPath $from -Destination $to -Force }
    catch {
        Fail "Could not write $to. Close the game, or run PowerShell as Administrator if the game is under Program Files. ($($_.Exception.Message))"
    }
}

function Install-VozAlien([string]$GameDir, [bool]$Uninstall) {
    Say 'VozAlien installer'
    if (-not $GameDir) { $GameDir = Find-GameDir }
    if (-not $GameDir) { Fail 'Alien: Isolation was not found in your Steam libraries. Run again with -GameDir "<folder with AI.exe>".' }
    $exe = Join-Path $GameDir 'AI.exe'
    if (-not (Test-Path -LiteralPath $exe)) { Fail "AI.exe not found in: $GameDir" }
    Info "game: $GameDir"

    if ($Uninstall) {
        Remove-Item -LiteralPath (Join-Path $GameDir 'VozAlien.asi') -Force -ErrorAction SilentlyContinue
        Say 'VozAlien removed (VozAlien.ini and VozAlien.log were kept; the ASI loader was left in place).'
        return
    }

    if ((Get-Sha $exe) -ne $SteamSha) {
        Warn 'AI.exe is not the Steam build VozAlien was made for. The mod checks the game code at startup'
        Warn 'and stays disabled if it does not match (it will say so in VozAlien.log).'
    }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('vozalien-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    try {
        Say 'Downloading the latest release'
        foreach ($f in @('VozAlien.asi', 'VozAlien.ini')) {
            Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/$Repo/releases/latest/download/$f" -OutFile (Join-Path $tmp $f)
        }

        # The older AlienIsolationVoiceRestore mod would emit its own events on top of ours.
        $old = Join-Path $GameDir 'AlienIsolationVoiceRestore.asi'
        if (Test-Path -LiteralPath $old) {
            Warn 'AlienIsolationVoiceRestore.asi found: renaming it to .asi.disabled so both mods do not run together.'
            Move-Item -LiteralPath $old -Destination "$old.disabled" -Force
        }

        $loader = $null
        foreach ($n in $LoaderNames) {
            $p = Join-Path $GameDir $n
            if ((Test-Path -LiteralPath $p) -and (Test-Ual $p)) { $loader = $n; break }
        }
        if ($loader) {
            Info "ASI loader already installed: $loader"
        } else {
            if (-not (Test-Path -LiteralPath (Join-Path $GameDir 'd3d11.dll'))) { $loader = 'd3d11.dll' }
            elseif (-not (Test-Path -LiteralPath (Join-Path $GameDir 'winmm.dll'))) { $loader = 'winmm.dll' }
            else { Fail 'No ASI loader found and both d3d11.dll and winmm.dll are taken by other mods. Install Ultimate ASI Loader manually.' }
            Say "Installing Ultimate ASI Loader v10.0.1 (ThirteenAG, MIT) as $loader"
            $zip = Join-Path $tmp 'ual.zip'
            Invoke-WebRequest -UseBasicParsing -Uri $UalUrl -OutFile $zip
            if ((Get-Sha $zip) -ne $UalZipSha) { Fail 'Ultimate ASI Loader archive checksum mismatch.' }
            Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $tmp 'ual') -Force
            $dll = Join-Path (Join-Path $tmp 'ual') 'dinput8.dll'
            if ((Get-Sha $dll) -ne $UalDllSha) { Fail 'Ultimate ASI Loader DLL checksum mismatch.' }
            Copy-Safe $dll (Join-Path $GameDir $loader)
        }

        Say 'Installing VozAlien'
        Copy-Safe (Join-Path $tmp 'VozAlien.asi') (Join-Path $GameDir 'VozAlien.asi')
        $ini = Join-Path $GameDir 'VozAlien.ini'
        if (Test-Path -LiteralPath $ini) { Info 'kept your existing VozAlien.ini' }
        else { Copy-Safe (Join-Path $tmp 'VozAlien.ini') $ini }

        Say 'Done.'
        Write-Host ''
        Write-Host '  First launch: stay quiet for ~3 seconds when the game window opens (room-noise calibration).'
        Write-Host '  Windows must allow desktop apps to use the microphone:'
        Write-Host '  Settings > Privacy & security > Microphone > "Let desktop apps access your microphone".'
        Write-Host "  Log: $(Join-Path $GameDir 'VozAlien.log')"
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Install-VozAlien -GameDir $GameDir -Uninstall ([bool]$Uninstall)
