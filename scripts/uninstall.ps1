<#
    uninstall.ps1 -- unregister the VR EyeCapture OpenXR implicit API layers (HKLM and HKCU): the 64-bit layer
    (XR_APILAYER_JRDEVO_eye_capture.json, 64-bit registry view) and the 32-bit layer for 32-bit games
    (XR_APILAYER_JRDEVO_eye_capture32.json, HKLM\SOFTWARE\WOW6432Node\..., the 32-bit view). Files and recordings are left
    in place. Elevates itself (UAC) when an HKLM entry exists. DEVELOPER SCRIPT, not in the release zip: users pick
    "Unregister from VR games..." in the tray menu (or run EyeCaptureTray.exe --unregister from an admin prompt).
      powershell -ExecutionPolicy Bypass -File scripts\uninstall.ps1
#>
param([switch]$Elevated)
$ErrorActionPreference = 'Stop'
$jsonName = 'XR_APILAYER_JRDEVO_eye_capture.json'
$jsonName32 = 'XR_APILAYER_JRDEVO_eye_capture32.json'
$lm = 'HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
$lm32 = 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit'
$cu = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

function Ours($path, [string[]]$names) {
    if (-not (Test-Path $path)) { return @() }
    return @((Get-Item $path).GetValueNames() | Where-Object { $f = [System.IO.Path]::GetFileName($_); $names -contains $f })
}

foreach ($n in (Ours $cu @($jsonName, $jsonName32))) { Remove-ItemProperty -Path $cu -Name $n; Write-Host "removed HKCU: $n" }

$lmNames = Ours $lm @($jsonName)
$lm32Names = Ours $lm32 @($jsonName32)
if ($lmNames.Count -gt 0 -or $lm32Names.Count -gt 0) {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        Write-Host 'Requesting administrator rights (UAC) to edit HKLM...'
        $p = Start-Process powershell -Verb RunAs -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated') -Wait -PassThru
        exit $p.ExitCode
    }
    foreach ($n in $lmNames) { Remove-ItemProperty -Path $lm -Name $n; Write-Host "removed HKLM: $n" }
    foreach ($n in $lm32Names) { Remove-ItemProperty -Path $lm32 -Name $n; Write-Host "removed HKLM (32-bit view): $n" }
}
Write-Host ''
& reg query 'HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path $lm32) { & reg query 'HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ApiLayers\Implicit' }
if ((Ours $lm @($jsonName)).Count -eq 0 -and (Ours $lm32 @($jsonName32)).Count -eq 0 -and (Ours $cu @($jsonName, $jsonName32)).Count -eq 0) {
    Write-Host 'VR EyeCapture OpenXR layers (64-bit and 32-bit) unregistered.' -ForegroundColor Green
    exit 0
}
Write-Host 'some registrations remain' -ForegroundColor Red
exit 1
