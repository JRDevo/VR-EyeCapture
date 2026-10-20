<#
    install.ps1 -- register the VR EyeCapture OpenXR implicit API layer. DEVELOPER SCRIPT (building from source); it is not
    in the release zip. Users just run EyeCaptureTray.exe: it registers the layer itself (one UAC prompt, the same
    rules as this script; tray/install.cpp) and unregisters it from its tray menu. Command-line equivalent from an
    admin prompt: EyeCaptureTray.exe --register / --unregister.

    Writes  HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit\<full path of XR_APILAYER_JRDEVO_eye_capture.json> = DWORD 0
    (0 = enabled), the same way Virtual Desktop registers its own layer, and verifies it with `reg query`.
    Elevates itself (UAC) for HKLM. -CurrentUser registers under HKCU instead (no elevation; the OpenXR loader reads
    both hives, but HKCU is ignored for games that run elevated).

    Run from the repo: copies build\Release\{eye_capture.dll, XR_APILAYER_JRDEVO_eye_capture.json, EyeCaptureTray.exe,
    EyeCaptureMirror.exe, EyeCaptureRecorder.exe} and build32\Release\{eye_capture32.dll, XR_APILAYER_JRDEVO_eye_capture32.json}
    next to config.ini in the repo root first, and build\Release\openvr_api.dll to steamvr\openvr_api.dll (the SteamVR
    proxy that the tray's "SteamVR games..." copies into a game folder; the same place as in the release zip). Run from an unzipped package: registers the files where they are.
    Any older registration of a json with the same file name in another folder is removed.

    32-bit OpenXR games (docs/X86_DESIGN.md): when eye_capture32.dll + XR_APILAYER_JRDEVO_eye_capture32.json are next to
    config.ini, that json is registered the same way in the 32-bit registry view, the one the 32-bit OpenXR loader reads:
        HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit\<full path of XR_APILAYER_JRDEVO_eye_capture32.json> = 0
    (run this script from the normal 64-bit PowerShell). -CurrentUser does not register the 32-bit layer: HKCU\Software is the
    same key for 32- and 64-bit programs, so both loaders would see both manifests there.

    Windows PowerShell 5.1 compatible.
      powershell -ExecutionPolicy Bypass -File scripts\install.ps1 [-CurrentUser]
#>
param([switch]$CurrentUser, [switch]$NoCopy, [switch]$Elevated)
$ErrorActionPreference = 'Stop'
$jsonName = 'XR_APILAYER_JRDEVO_eye_capture.json'
$jsonName32 = 'XR_APILAYER_JRDEVO_eye_capture32.json'
$regPath32 = 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit'
$regPath32Cmd = 'HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit'
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
            foreach ($n in ((Get-Item $mine).GetValueNames() | Where-Object { $f = [System.IO.Path]::GetFileName($_); $f -ieq $jsonName -or $f -ieq $jsonName32 })) {
                Remove-ItemProperty -Path $mine -Name $n -ErrorAction SilentlyContinue
                Write-Host "removed per-user duplicate registration: $n"
            }
        }
        & reg query $regPathCmd
        if (Test-Path $regPath32) { & reg query $regPath32Cmd }
        exit 0
    }
}

# --- stage files next to config.ini (repo layout) ---
$build = Join-Path $root 'build\Release'
if (-not $NoCopy -and (Test-Path (Join-Path $build 'eye_capture.dll'))) {
    # (quick mode loads SteamVR's own openvr_api.dll: nothing of Valve's is staged)
    foreach ($f in @('eye_capture.dll', $jsonName, 'EyeCaptureTray.exe', 'EyeCaptureMirror.exe', 'EyeCaptureRecorder.exe')) {
        $src = Join-Path $build $f
        if (-not (Test-Path $src)) { continue }
        try { Copy-Item -LiteralPath $src -Destination (Join-Path $root $f) -Force }
        catch { Fail "could not copy $f (is a game or the tray app using it? close it and retry): $($_.Exception.Message)" }
    }
    # the 32-bit layer (32-bit OpenXR games) from the Win32 build (build32), when it was built
    $build32 = Join-Path $root 'build32\Release'
    foreach ($f in @('eye_capture32.dll', $jsonName32)) {
        $src = Join-Path $build32 $f
        if (-not (Test-Path $src)) { continue }
        try { Copy-Item -LiteralPath $src -Destination (Join-Path $root $f) -Force }
        catch { Fail "could not copy $f (is a 32-bit game using it? close it and retry): $($_.Exception.Message)" }
    }
    if (-not (Test-Path (Join-Path $build32 'eye_capture32.dll'))) { Write-Host 'note: build32\Release\eye_capture32.dll not built -- 32-bit games are not set up (cmake -B build32 -A Win32)' -ForegroundColor Yellow }
    # the SteamVR proxy goes into steamvr\ (as in the zip; the tray's "SteamVR games" copies it into a game folder from there)
    $proxy = Join-Path $build 'openvr_api.dll'
    if (Test-Path $proxy) {
        New-Item -ItemType Directory -Force (Join-Path $root 'steamvr') | Out-Null
        try { Copy-Item -LiteralPath $proxy -Destination (Join-Path $root 'steamvr\openvr_api.dll') -Force }
        catch { Fail "could not copy steamvr\openvr_api.dll: $($_.Exception.Message)" }
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
    $ok = (Get-Item $other).GetValueNames() | Where-Object { $f = [System.IO.Path]::GetFileName($_); $f -ieq $jsonName -or $f -ieq $jsonName32 }
    foreach ($n in $ok) {
        try { Remove-ItemProperty -Path $other -Name $n; Write-Host "removed duplicate registration in the other hive: $n" }
        catch { Write-Host "note: $n is also registered in $other (could not remove: run uninstall from that context)" -ForegroundColor Yellow }
    }
}

Write-Host ''
& reg query $regPathCmd /v $json
if ($LASTEXITCODE -ne 0) { Fail 'registry verification failed' }

# --- 32-bit games: the 32-bit layer in the 32-bit registry view (HKLM only) ---
$json32 = Join-Path $root $jsonName32
if ((Test-Path $json32) -and (Test-Path (Join-Path $root 'eye_capture32.dll'))) {
    if ($CurrentUser) {
        Write-Host 'note: -CurrentUser: the 32-bit layer is not registered (HKLM only; HKCU is shared by 32- and 64-bit loaders)' -ForegroundColor Yellow
    } else {
        $json32 = (Resolve-Path $json32).Path
        if (-not (Test-Path $regPath32)) { New-Item -Path $regPath32 -Force | Out-Null }
        foreach ($name in (Get-Item -Path $regPath32).GetValueNames()) {
            if ($name -ne $json32 -and ([System.IO.Path]::GetFileName($name) -ieq $jsonName32)) {
                Remove-ItemProperty -Path $regPath32 -Name $name
                Write-Host "removed stale 32-bit registration: $name"
            }
        }
        New-ItemProperty -Path $regPath32 -Name $json32 -PropertyType DWord -Value 0 -Force | Out-Null
        & reg query $regPath32Cmd /v $json32
        if ($LASTEXITCODE -ne 0) { Fail '32-bit registry verification failed' }
        Write-Host "Installed for 32-bit games: $json32 (HKLM, 32-bit view)" -ForegroundColor Green
    }
}
Write-Host ''
Write-Host "Installed: $json ($hive)" -ForegroundColor Green
Write-Host 'Start any OpenXR game; press the hotkey from config.ini (default ScrollLock) to start / stop recording.'
Write-Host "Required: run $(Join-Path $root 'EyeCaptureTray.exe') BEFORE the game (require_tray = 1: without the tray the layer stays dormant)."
if ($Elevated) { Start-Sleep -Seconds 2 }
exit 0
