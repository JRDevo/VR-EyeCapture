<#
    run_install.ps1 -- tests for the layer registration (tray/install.cpp + EyeCaptureTray.exe --register / --unregister /
    --status + the tray's GUI flow). No GPU, no game, no admin: every registry access is redirected to test keys under
    HKCU\Software\OpenXR-EyeCapture-Test (EYECAP_TRAY_TEST_REG_ROOTS), deleted at the start and the end. The real
    HKLM / HKCU OpenXR keys are only READ (step 4) and must be unchanged at the end.
      powershell -ExecutionPolicy Bypass -File tests\run_install.ps1
    1. install_test.exe : unit test of tray/install.cpp (register / stale / HKCU duplicate / unregister / files missing /
                          read-only key = access denied).
    2. CLI on test keys  : a scratch copy of EyeCaptureTray.exe in build\tt3 (empty json + dll next to it):
                          --status registered=no -> stale value -> registered=stale -> --register (exit 0, exactly one
                          value = tt3\json, stale gone) -> CU duplicate -> registered=yes hkcu_duplicate=yes -> --register
                          removes it -> --unregister (exit 0) -> registered=no; dll missing -> --register exit 1 and
                          files=missing; read-only LM key -> --register exit 5. tray.log has the --register lines.
    3. GUI on test keys  : the scratch tray in test mode (EYECAP_TRAY_TEST_SHM + EYECAP_TRAY_TEST_REG=1): logs the
                          registration state, the start question (answered No in test mode: no dialog, no UAC), the
                          "Not registered for VR games" tooltip; a stale value gives the "registered from another folder"
                          question; a --register from outside is picked up by the 10 s re-query (tooltip back to normal).
    4. build\Release\EyeCaptureTray.exe --status on the REAL keys (read-only): registered=stale|no; the real keys unchanged.
    Refuses to run elevated (an elevated --register could write the real HKLM if the test roots were ignored).
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$unit = Join-Path $bin 'install_test.exe'
$trayBin = Join-Path $bin 'EyeCaptureTray.exe'
foreach ($f in @($unit, $trayBin)) { if (-not (Test-Path $f)) { throw "build first: $f" } }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($admin) { throw 'run this test from a NON-elevated prompt' }
$fail = 0
function Step($name, $ok) { Write-Host ("{0}: {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' }); if (-not $ok) { $script:fail++ } }
$jsonName = 'XR_APILAYER_JRDEVO_eye_capture.json'
$testKey = 'HKCU:\Software\OpenXR-EyeCapture-Test'
$lmKey = "$testKey\LM"; $cuKey = "$testKey\CU"
$rootsEnv = 'HKCU\Software\OpenXR-EyeCapture-Test\LM;HKCU\Software\OpenXR-EyeCapture-Test\CU'
function Ours($key) {
    if (-not (Test-Path $key)) { return @() }
    return @((Get-Item $key).GetValueNames() | Where-Object { [System.IO.Path]::GetFileName($_) -ieq $jsonName })
}
function SetVal($key, $name) { if (-not (Test-Path $key)) { New-Item -Path $key -Force | Out-Null }; New-ItemProperty -Path $key -Name $name -PropertyType DWord -Value 0 -Force | Out-Null }
function RealKeys {
    $s = ''
    foreach ($k in @('HKLM:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit', 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit')) {
        if (Test-Path $k) { $i = Get-Item $k; foreach ($n in $i.GetValueNames()) { $s += "$k|$n|$($i.GetValue($n))`n" } }
    }
    return $s
}
# runs the scratch tray's CLI, returns @{ code; out }
function TrayCli($exe, $arg) {
    $o = Join-Path $env:TEMP "eyecap_install_cli_$PID.txt"
    $p = Start-Process -FilePath $exe -ArgumentList $arg -NoNewWindow -Wait -PassThru -RedirectStandardOutput $o
    $text = (Get-Content $o -Raw -ErrorAction SilentlyContinue); if ($null -eq $text) { $text = '' }
    Remove-Item $o -ErrorAction SilentlyContinue
    $text = $text.Trim()
    Write-Host ("  {0} -> exit {1}: {2}" -f $arg, $p.ExitCode, $text)
    return @{ code = $p.ExitCode; out = $text }
}

$realBefore = RealKeys

# ---- 1. unit test
& $unit; Step 'install_test' ($LASTEXITCODE -eq 0)

# ---- 2. CLI on test keys
$t = Join-Path $root 'build\tt3'
Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory (Join-Path $t 'appdata') | Out-Null
Copy-Item $trayBin $t
Copy-Item (Join-Path $root 'config.ini') $t
New-Item -ItemType File (Join-Path $t $jsonName) | Out-Null
New-Item -ItemType File (Join-Path $t 'eye_capture.dll') | Out-Null
$exe = Join-Path $t 'EyeCaptureTray.exe'
$ourName = Join-Path $t $jsonName
$stale = "D:\Old Folder\$jsonName"
& $unit cleanup | Out-Null
$oldAppdata = $env:APPDATA
$gui = $null
try {
    $env:APPDATA = Join-Path $t 'appdata'
    $env:EYECAP_TRAY_TEST_REG_ROOTS = $rootsEnv
    $r = TrayCli $exe '--status'
    # guard: the real HKLM says "stale" for this folder; "no" proves the test roots are in effect
    if ($r.out -notmatch 'registered=no') { throw "test registry roots not in effect (got '$($r.out)'); stopping before any --register" }
    Step 'CLI --status on empty test keys: registered=no' $true
    SetVal $lmKey $stale
    Step 'CLI --status with a stale value: registered=stale' ((TrayCli $exe '--status').out -match 'registered=stale')
    $r = TrayCli $exe '--register'
    Step 'CLI --register: exit 0, prints the value' ($r.code -eq 0 -and $r.out -match [regex]::Escape("registered: $ourName = 0"))
    $o = @(Ours $lmKey)
    Step 'test LM holds exactly <tt3>\json = 0, stale removed' ($o.Count -eq 1 -and $o[0] -eq $ourName -and (Get-ItemProperty $lmKey).$ourName -eq 0)
    Step 'CLI --status: registered=yes' ((TrayCli $exe '--status').out -match 'registered=yes(?! hkcu)')
    SetVal $cuKey $ourName
    Step 'CLI --status with a CU copy: registered=yes hkcu_duplicate=yes' ((TrayCli $exe '--status').out -match 'registered=yes hkcu_duplicate=yes')
    $r = TrayCli $exe '--register'
    Step 'CLI --register again: exit 0, CU copy removed' ($r.code -eq 0 -and (Ours $cuKey).Count -eq 0)
    $r = TrayCli $exe '--unregister'
    Step 'CLI --unregister: exit 0, no same-name value left' ($r.code -eq 0 -and (Ours $lmKey).Count -eq 0)
    Step 'CLI --status: registered=no' ((TrayCli $exe '--status').out -match 'registered=no')
    Rename-Item (Join-Path $t 'eye_capture.dll') 'eye_capture.dll.off'
    $r = TrayCli $exe '--register'
    Step 'CLI --register without the dll: exit 1, error printed, nothing written' ($r.code -eq 1 -and $r.out -match 'not found next to EyeCaptureTray.exe' -and (Ours $lmKey).Count -eq 0)
    Step 'CLI --status without the dll: files=missing' ((TrayCli $exe '--status').out -match 'files=missing')
    Rename-Item (Join-Path $t 'eye_capture.dll.off') 'eye_capture.dll'
    & $unit lockdemo | Out-Null
    $env:EYECAP_TRAY_TEST_REG_ROOTS = 'HKCU\Software\OpenXR-EyeCapture-Test\RO;HKCU\Software\OpenXR-EyeCapture-Test\CU'
    $r = TrayCli $exe '--register'
    Step 'CLI --register on a read-only key: exit 5 (access denied)' ($r.code -eq 5 -and $r.out -match 'needs administrator rights')
    $env:EYECAP_TRAY_TEST_REG_ROOTS = $rootsEnv
    & $unit cleanup | Out-Null
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Step 'tray.log has the --register / --unregister lines' ($log -match 'tray: --register \(exe folder' -and $log -match 'tray: registered: ' -and $log -match 'tray: unregistered: ' -and $log -match 'tray: --register failed: .*needs administrator rights')

    # ---- 3. GUI flow on test keys
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-install-test'; $env:EYECAP_TRAY_TEST_REG = '1'
    Remove-Item (Join-Path $t 'logs\tray.log')
    $gui = Start-Process $exe -PassThru
    Start-Sleep -Seconds 3
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Step 'GUI: registration state logged (not registered)' ($log -match 'tray: registration: not registered')
    Step 'GUI: start question logged, answered No in test mode' ($log -match 'question box: OpenXR-EyeCapture is not registered for VR games yet\.[\s\S]*Register it now\? Windows will ask for administrator rights once\.[\s\S]*\(Later: right-click the tray icon -> Register for VR games\.\) -> No \(test mode\)')
    Step 'GUI: tooltip first line "Not registered for VR games"' ($log -match 'tooltip now: Not registered for VR games \(right-click: Register\)\s+OpenXR-EyeCapture: no OpenXR game running')
    Stop-Process -Id $gui.Id; $gui = $null
    Start-Sleep -Milliseconds 500
    SetVal $lmKey $stale
    Remove-Item (Join-Path $t 'logs\tray.log')
    $gui = Start-Process $exe -PassThru
    Start-Sleep -Seconds 3
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Step 'GUI: stale value -> "registered from another folder" question' ($log -match 'registration: registered from another folder \(D:\\Old Folder\\' -and $log -match 'question box: OpenXR-EyeCapture is registered from another folder:\s+D:\\Old Folder\\XR_APILAYER_JRDEVO_eye_capture\.json\s+Re-register it from this folder\? Windows will ask for administrator rights once\. -> No')
    Remove-Item Env:\EYECAP_TRAY_TEST_SHM; Remove-Item Env:\EYECAP_TRAY_TEST_REG
    $r = TrayCli $exe '--register'
    $ok = $false
    for ($i = 0; $i -lt 30 -and -not $ok; $i++) {
        Start-Sleep -Milliseconds 500
        $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
        $ok = $log -match 'tray: registration: registered \(' -and $log -match 'tooltip now: OpenXR-EyeCapture: no OpenXR game running'
    }
    Step 'GUI: picks up an outside --register within 15 s (re-query), tooltip back to normal' $ok
} finally {
    $env:APPDATA = $oldAppdata
    foreach ($v in @('EYECAP_TRAY_TEST_REG_ROOTS', 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_REG')) { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
    if ($gui -and -not $gui.HasExited) { Stop-Process -Id $gui.Id }
    & $unit cleanup | Out-Null
}
Write-Host '--- tray.log (registration lines):'
(Get-Content (Join-Path $t 'logs\tray.log') -Encoding UTF8) | Where-Object { $_ -match 'regist|question|tooltip now|TEST registry' } | ForEach-Object { Write-Host "  $_" }

# ---- 4. the real keys: read-only status from build\Release
$r = TrayCli $trayBin '--status'
Step 'build\Release --status on the real keys: registered=stale or no' ($r.out -match 'registered=(stale|no)')
Step 'real HKLM / HKCU OpenXR keys unchanged' ((RealKeys) -eq $realBefore)
Step 'test key removed' (-not (Test-Path $testKey))
if ($fail) { exit 1 } else { exit 0 }
