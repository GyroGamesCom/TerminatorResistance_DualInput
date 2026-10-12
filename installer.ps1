# Dual Input mod for Terminator: Resistance - installer / uninstaller.
# Run through install.bat or uninstall.bat.
param(
    [switch]$Uninstall,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$AppId      = '954740'
$GameFolder = 'Terminator Resistance'
$TargetSub  = 'Terminator\Binaries\Win64'
$ExeName    = 'Terminator-Win64-Shipping.exe'
$ModFiles   = @('xinput1_3.dll')
$IniName    = 'xinput_mouse.ini'
$LogNames   = @('mixed_input.log', 'mixed_input.old.log')
$BackupDir  = 'DualInput_backup'
# SHA-256 of the Steam build the mod was made and tested on (game version 1.060).
$KnownExe   = '5A01BBEE102990B1B1108C8656994ED19D46A9B2735C3B3A11EB9C7921CAFDB4'
$ModDir     = Join-Path $PSScriptRoot 'mod'
# The game's user Input.ini: the mod turns off UE4 mouse smoothing there (lag on gyro).
$UserInputIni = Join-Path $env:LOCALAPPDATA 'Terminator\Saved\Config\WindowsNoEditor\Input.ini'
$SmoothMarker = '; DualInput: mouse smoothing off'
$SmoothBlock  = "$SmoothMarker`r`n[/Script/Engine.InputSettings]`r`nbEnableMouseSmoothing=False`r`n"

# Returns the Win64 folder (the one with Terminator-Win64-Shipping.exe), or $null.
function Resolve-Target($dir) {
    if (-not $dir) { return $null }
    foreach ($d in $dir, (Join-Path $dir $TargetSub)) {
        if (Test-Path (Join-Path $d $ExeName)) { return $d }
    }
    return $null
}

function Find-GameDir {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($steam) { break }
        } catch {}
    }
    if (-not $steam) { return $null }

    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += $m.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libs | Select-Object -Unique) {
        $apps = Join-Path $lib 'steamapps'
        # Prefer the folder named in the game's Steam manifest, fall back to the default name.
        $names = @()
        $acf = Join-Path $apps "appmanifest_$AppId.acf"
        if (Test-Path $acf) {
            $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $names += $m.Groups[1].Value }
        }
        $names += $GameFolder
        foreach ($n in $names | Select-Object -Unique) {
            $t = Resolve-Target (Join-Path $apps "common\$n")
            if ($t) { return $t }
        }
    }
    return $null
}

Write-Host ''
Write-Host '=== Dual Input mod for Terminator: Resistance ===' -ForegroundColor Cyan
Write-Host ''

$GameDir = Resolve-Target $GameDir
if (-not $GameDir) { $GameDir = Find-GameDir }
while (-not $GameDir) {
    Write-Host 'The game was not found automatically (or the folder is wrong).' -ForegroundColor Yellow
    $in = (Read-Host "Paste the game folder (the one with $GameFolder, or Terminator\Binaries\Win64), or leave empty to cancel").Trim('" ')
    if (-not $in) { Write-Host 'Cancelled.'; exit 1 }
    $GameDir = Resolve-Target $in
    if (-not $GameDir) { Write-Host "$ExeName not found in: $in" -ForegroundColor Yellow }
}
Write-Host "Install folder: $GameDir"

$backup = Join-Path $GameDir $BackupDir

if ($Uninstall) {
    foreach ($f in $ModFiles + $LogNames) {
        $path = Join-Path $GameDir $f
        if (Test-Path $path) { Remove-Item $path -Force; Write-Host "  removed  $f" }
    }
    if (Test-Path $backup) {
        Get-ChildItem $backup | ForEach-Object {
            Move-Item $_.FullName (Join-Path $GameDir $_.Name) -Force
            Write-Host "  restored $($_.Name) (from before the mod)"
        }
        Remove-Item $backup -Force -Recurse
    }
    if (Test-Path $UserInputIni) {
        $txt = Get-Content $UserInputIni -Raw
        if ($txt -and $txt.Contains($SmoothMarker)) {
            Set-Content $UserInputIni ($txt.Replace($SmoothBlock, '')) -NoNewline -Encoding ASCII
            Write-Host '  restored mouse smoothing setting (Input.ini)'
        }
    }
    $ini = Join-Path $GameDir $IniName
    if (Test-Path $ini) {
        $a = Read-Host "Delete your settings file $IniName too? (y/N)"
        if ($a -match '^[yY]') { Remove-Item $ini -Force; Write-Host "  removed  $IniName" }
        else { Write-Host "  kept     $IniName" }
    }
    Write-Host ''
    Write-Host 'Mod uninstalled.' -ForegroundColor Green
    exit 0
}

$hash = (Get-FileHash (Join-Path $GameDir $ExeName) -Algorithm SHA256).Hash
if ($hash -ne $KnownExe) {
    Write-Host ''
    Write-Host "WARNING: your $ExeName is a different version than the one the mod" -ForegroundColor Yellow
    Write-Host 'was made for (Steam, game version 1.060). The mod checks the game code before' -ForegroundColor Yellow
    Write-Host 'patching it and skips anything that does not match. Stick priority still works,' -ForegroundColor Yellow
    Write-Host 'but gamepad glyphs / linear gyro may not.' -ForegroundColor Yellow
    $a = Read-Host 'Install anyway? (y/N)'
    if ($a -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }
}

foreach ($f in $ModFiles) {
    $src = Join-Path $ModDir $f
    $dst = Join-Path $GameDir $f
    if ((Test-Path $dst) -and (Get-FileHash $dst).Hash -ne (Get-FileHash $src).Hash) {
        # Another mod (or an older copy) uses the same file name: keep it, unless it is
        # an earlier version of this mod (it then reads xinput_mouse.ini).
        $isOurs = Select-String -Path $dst -Pattern $IniName -SimpleMatch -Quiet
        if (-not $isOurs -and -not (Test-Path (Join-Path $backup $f))) {
            New-Item -ItemType Directory -Path $backup -Force | Out-Null
            Copy-Item $dst (Join-Path $backup $f)
            Write-Host "  backed up existing $f to $BackupDir\" -ForegroundColor Yellow
        }
    }
    Copy-Item $src $dst -Force
    Write-Host "  installed $f"
}

$ini = Join-Path $GameDir $IniName
if (Test-Path $ini) {
    Write-Host "  kept your existing $IniName (settings unchanged)"
} else {
    Copy-Item (Join-Path $ModDir $IniName) $ini
    Write-Host "  installed $IniName"
}

# Turn off UE4 mouse smoothing (the game ships with it on; it adds lag to gyro).
$txt = if (Test-Path $UserInputIni) { Get-Content $UserInputIni -Raw } else { '' }
if (-not $txt) { $txt = '' }
if ($txt.Contains($SmoothMarker)) {
    Write-Host '  mouse smoothing already off (Input.ini)'
} elseif ($txt -match 'bEnableMouseSmoothing') {
    Write-Host '  Input.ini already sets bEnableMouseSmoothing, left unchanged' -ForegroundColor Yellow
} else {
    New-Item -ItemType Directory -Path (Split-Path $UserInputIni) -Force | Out-Null
    if ($txt.Trim()) { $txt = $txt.TrimEnd() + "`r`n" } else { $txt = '' }
    Set-Content $UserInputIni ($txt + $SmoothBlock) -NoNewline -Encoding ASCII
    Write-Host '  turned off mouse smoothing (Input.ini)'
}

Write-Host ''
Write-Host 'Done. Next steps:' -ForegroundColor Green
Write-Host '  1. Steam Input: gyro -> "As Mouse", right trackpad -> "Joystick" (see README).'
Write-Host "  2. Start the game. Settings: $ini"
Write-Host '  3. To remove the mod, run uninstall.bat.'
