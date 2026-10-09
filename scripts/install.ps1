<#
    install.ps1 -- register the OpenXR-EyeCapture implicit API layer.

    Writes  HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit\<full path of XR_APILAYER_JRDEVO_eye_capture.json> = DWORD 0
    (0 = enabled), the same way Virtual Desktop registers its own layer, and verifies it with `reg query`.
    Elevates itself (UAC) for HKLM. -CurrentUser registers under HKCU instead (no elevation; the OpenXR loader reads
    both hives, but HKCU is ignored for games that run elevated).

    Run from the repo: copies build\Release\{eye_capture.dll, XR_APILAYER_JRDEVO_eye_capture.json, EyeCaptureTray.exe}
    next to config.ini in the repo root first. Run from an unzipped package: registers the files where they are.
    Any older registration of a json with the same file name in another folder is removed.

    Windows PowerShell 5.1 compatible.
      powershell -ExecutionPolicy Bypass -File scripts\install.ps1 [-CurrentUser]
#>
param([switch]$CurrentUser, [switch]$NoCopy, [switch]$Elevated)
$ErrorActionPreference = 'Stop'
$jsonName = 'XR_APILAYER_JRDEVO_eye_capture.json'
$root = Split-Path -Parent $PSScriptRoot
$hive = if ($CurrentUser) { 'HKCU' } else { 'HKLM' }
$regPath = "${hive}:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
$regPathCmd = "$hive\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"

function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; if ($Elevated) { Read-Host 'Press Enter' | Out-Null }; exit 1 }

# --- elevate for HKLM ---
if (-not $CurrentUser) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        Write-Host 'Requesting administrator rights (UAC) to write HKLM...'
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
        if ($NoCopy) { $a += '-NoCopy' }
        $p = Start-Process powershell -Verb RunAs -ArgumentList $a -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Host "elevated install failed (exit $($p.ExitCode))" -ForegroundColor Red; exit $p.ExitCode }
        # The elevated process may run as a different account, so its HKCU is not ours: remove OUR per-user duplicate
        # here, or the official loader loads the layer twice (double negotiate -> crash in games that use that loader).
        $mine = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
        if (Test-Path $mine) {
            foreach ($n in ((Get-Item $mine).GetValueNames() | Where-Object { [System.IO.Path]::GetFileName($_) -ieq $jsonName })) {
                Remove-ItemProperty -Path $mine -Name $n -ErrorAction SilentlyContinue
                Write-Host "removed per-user duplicate registration: $n"
            }
        }
        & reg query $regPathCmd
        exit 0
    }
}

# --- stage files next to config.ini (repo layout) ---
$build = Join-Path $root 'build\Release'
if (-not $NoCopy -and (Test-Path (Join-Path $build 'eye_capture.dll'))) {
    foreach ($f in @('eye_capture.dll', $jsonName, 'EyeCaptureTray.exe')) {
        $src = Join-Path $build $f
        if (-not (Test-Path $src)) { continue }
        try { Copy-Item -LiteralPath $src -Destination (Join-Path $root $f) -Force }
        catch { Fail "could not copy $f (is a game or the tray app using it? close it and retry): $($_.Exception.Message)" }
    }
    Write-Host "copied the Release build into $root"
}
$json = Join-Path $root $jsonName
if (-not (Test-Path $json)) { Fail "$json not found (build first, or run from the unzipped package)" }
if (-not (Test-Path (Join-Path $root 'eye_capture.dll'))) { Fail "eye_capture.dll not found next to $json" }
$json = (Resolve-Path $json).Path

# --- registry ---
if (-not (Test-Path $regPath)) { New-Item -Path $regPath -Force | Out-Null }
$key = Get-Item -Path $regPath
foreach ($name in $key.GetValueNames()) {
    if ($name -ne $json -and ([System.IO.Path]::GetFileName($name) -ieq $jsonName)) {
        Remove-ItemProperty -Path $regPath -Name $name
        Write-Host "removed stale registration: $name"
    }
}
New-ItemProperty -Path $regPath -Name $json -PropertyType DWord -Value 0 -Force | Out-Null
# the same json registered in the other hive would load the layer twice
$other = if ($CurrentUser) { 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit' } else { 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit' }
if (Test-Path $other) {
    $ok = (Get-Item $other).GetValueNames() | Where-Object { [System.IO.Path]::GetFileName($_) -ieq $jsonName }
    foreach ($n in $ok) {
        try { Remove-ItemProperty -Path $other -Name $n; Write-Host "removed duplicate registration in the other hive: $n" }
        catch { Write-Host "note: $n is also registered in $other (could not remove: run uninstall from that context)" -ForegroundColor Yellow }
    }
}

Write-Host ''
& reg query $regPathCmd /v $json
if ($LASTEXITCODE -ne 0) { Fail 'registry verification failed' }
Write-Host ''
Write-Host "Installed: $json ($hive)" -ForegroundColor Green
Write-Host 'Start any OpenXR game; press the hotkey from config.ini (default Ctrl+Shift+F10) to start / stop recording.'
Write-Host "Optional: run $(Join-Path $root 'EyeCaptureTray.exe') for a tray icon."
if ($Elevated) { Start-Sleep -Seconds 2 }
exit 0
