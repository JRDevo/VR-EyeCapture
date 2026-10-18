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
    3c. installed copy (installed.txt next to the exe; LOCALAPPDATA / APPDATA / XR_EYE_CAPTURE_TEST_VIDEOS = scratch folders):
                          tray.log in %LOCALAPPDATA%\VR EyeCapture\logs, config.ini seeded to %APPDATA%\VR EyeCapture, the
                          default recordings folder <Videos>\VR EyeCapture, nothing written next to the exe; --quit closes the
                          running tray of that folder; --uninstall-cleanup removes the (test) Run value, the (test) HKLM
                          registration and the (test) Home value.
    3b. start_with_windows = 1 in config.ini: the start restores the tray's OWN Run value when missing / another folder (test Run
                          key), never over a value switched off in Task Manager, never writes StartupApproved.
    3. GUI on test keys  : the start must not touch the startup registration: with a stale old-name "OpenXR-EyeCapture" Run value
                          and StartupApproved records in the test keys, the Run key and the records are UNCHANGED after the
                          start (one balloon + log line report it); %APPDATA%\OpenXR-EyeCapture\config.ini (scratch APPDATA)
                          is copied to %APPDATA%\VR EyeCapture (old kept). Then
                          the scratch tray in test mode (EYECAP_TRAY_TEST_SHM + EYECAP_TRAY_TEST_REG=1): logs the
                          registration state, the start question (answered No in test mode: no dialog, no UAC), the
                          "Not registered for VR games" tooltip; a stale value gives the "registered from another folder"
                          question; a --register from outside is picked up by the 10 s re-query (tooltip back to normal).
    4. build\Release\EyeCaptureTray.exe --status on the REAL keys (read-only): registered=stale|no; the real keys unchanged.
    Refuses to run elevated (an elevated --register could write the real HKLM if the test roots were ignored) unless
    -AllowElevated is passed (CI: the GitHub runner is always an admin and is thrown away; the "real keys unchanged"
    and "real HKLM says no" guards still run).
#>
param([switch]$AllowElevated)   # CI only (throw-away GitHub runner, always elevated): see docs/DEVELOPMENT.md "CI"
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$unit = Join-Path $bin 'install_test.exe'
$trayBin = Join-Path $bin 'EyeCaptureTray.exe'
foreach ($f in @($unit, $trayBin)) { if (-not (Test-Path $f)) { throw "build first: $f" } }
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if ($admin -and -not $AllowElevated) { throw 'run this test from a NON-elevated prompt (or pass -AllowElevated on a throw-away CI runner)' }
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
    $env:EYECAP_TRAY_TEST_RUN_KEY = 'HKCU\Software\OpenXR-EyeCapture-Test\Run'   # "Start with Windows" Run key redirected too
    $r = TrayCli $exe '--status'
    # guard: the real HKLM says "stale" for this folder; "no" proves the test roots are in effect
    if ($r.out -notmatch 'registered=no') { throw "test registry roots not in effect (got '$($r.out)'); stopping before any --register" }
    Step 'CLI --status on empty test keys: registered=no' $true
    Step 'CLI --status on empty test keys: autostart=no' ($r.out -match 'autostart=no')
    $runKey = 'HKCU:\Software\OpenXR-EyeCapture-Test\Run'
    New-Item -Path $runKey -Force | Out-Null
    New-ItemProperty -Path $runKey -Name 'VR EyeCapture' -Value 'x' -PropertyType String -Force | Out-Null
    Step 'CLI --status with a test Run value: autostart=yes' ((TrayCli $exe '--status').out -match 'autostart=yes')
    Remove-Item -Path $runKey -Recurse -Force
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
    # Antivirus behaviour rules (docs/DEVELOPMENT.md): the tray must not touch the startup registration at start. A stale old-name
    # "OpenXR-EyeCapture" Run value (test Run key) with StartupApproved records (test key) must stay EXACTLY as it is; the tray
    # only logs it and shows one balloon (logged in test mode). A config.ini in the old %APPDATA% folder is still copied once.
    $oldCmd = '"X:\Old Place\EyeCaptureTray.exe"'
    New-Item -Path $runKey -Force | Out-Null
    New-ItemProperty -Path $runKey -Name 'OpenXR-EyeCapture' -Value $oldCmd -PropertyType String -Force | Out-Null
    $apprKey = "$($runKey)Approved"
    New-Item -Path $apprKey -Force | Out-Null
    $recOld = [byte[]](3,0,0,0,0,0,0,0,0,0,0,0); $recNew = [byte[]](2,0,0,0,0,0,0,0,0,0,0,0)
    New-ItemProperty -Path $apprKey -Name 'OpenXR-EyeCapture' -Value $recOld -PropertyType Binary -Force | Out-Null
    New-ItemProperty -Path $apprKey -Name 'Other App' -Value $recNew -PropertyType Binary -Force | Out-Null
    function RunSnapshot { $a = Get-Item $runKey; $b = Get-Item $apprKey; (($a.GetValueNames() | Sort-Object | ForEach-Object { "$_=$($a.GetValue($_))" }) -join '|') + '#' + (($b.GetValueNames() | Sort-Object | ForEach-Object { "$_=" + (($b.GetValue($_)) -join ',') }) -join '|') }
    $snapBefore = RunSnapshot
    $oldCfgDir = Join-Path $t 'appdata\OpenXR-EyeCapture'; $newCfg = Join-Path $t 'appdata\VR EyeCapture\config.ini'
    New-Item -ItemType Directory -Force $oldCfgDir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $oldCfgDir 'config.ini'), "; old folder`r`n[capture]`r`nhotkey = ScrollLock`r`n", (New-Object System.Text.UTF8Encoding $false))
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-install-test'; $env:EYECAP_TRAY_TEST_REG = '1'
    Remove-Item (Join-Path $t 'logs\tray.log')
    $gui = Start-Process $exe -PassThru
    Start-Sleep -Seconds 3
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Step 'GUI: registration state logged (not registered)' ($log -match 'tray: registration: not registered')
    Step 'GUI: start question logged, answered No in test mode' ($log -match 'question box: VR EyeCapture is not registered for OpenXR games yet\.[\s\S]*Register VR EyeCapture for OpenXR games now\? Windows will ask for administrator rights once\.[\s\S]*\(Later: right-click the tray icon -> Register for VR games\.\) -> No \(test mode\)')
    Step 'GUI: tooltip first line "Not registered for VR games"' ($log -match 'tooltip now: Not registered for VR games \(right-click: Register\)\s+VR EyeCapture: no VR game running')
    $rk = Get-Item $runKey
    Step 'GUI start: the stale old-name Run value and every StartupApproved record are UNCHANGED' ((RunSnapshot) -eq $snapBefore -and -not ($rk.GetValueNames() -contains 'VR EyeCapture') -and $rk.GetValue('OpenXR-EyeCapture') -eq $oldCmd)
    Step 'GUI start: logs that start-with-Windows points elsewhere and left it unchanged' ($log -match [regex]::Escape('tray: start with Windows points at X:\Old Place\EyeCaptureTray.exe, not at this folder') -and $log -match 'left unchanged')
    Step 'GUI start: ONE balloon "Start with Windows still points to <old path>. Open Settings and tick ..."' ([regex]::Matches($log, [regex]::Escape('Start with Windows still points to X:\Old Place\EyeCaptureTray.exe. Open Settings and tick Start with Windows again to fix it.')).Count -eq 1)
    Step 'GUI start: no rename / sync / approval log lines (the old behaviour)' ($log -notmatch 'entry renamed|entry updated|StartupApproved record moved')
    Step 'GUI start: the old-name value shows as off (disabled in Task Manager) in the log and --status' ($log -match 'tray: start with Windows: off \(disabled in Task Manager' -and (TrayCli $exe '--status').out -match 'autostart=no')
    Step 'GUI: %APPDATA%\OpenXR-EyeCapture\config.ini copied to %APPDATA%\VR EyeCapture (old kept), logged' ((Test-Path $newCfg) -and (Test-Path (Join-Path $oldCfgDir 'config.ini')) -and ((Get-Content $newCfg -Raw) -eq (Get-Content (Join-Path $oldCfgDir 'config.ini') -Raw)) -and $log -match 'tray: config\.ini copied from the old folder .*OpenXR-EyeCapture\\config\.ini to .*VR EyeCapture\\config\.ini')
    Remove-Item -Path $runKey -Recurse -Force
    Remove-Item -Path "$($runKey)Approved" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force (Join-Path $t 'appdata\*')
    Stop-Process -Id $gui.Id; $gui = $null
    Start-Sleep -Milliseconds 500
    SetVal $lmKey $stale
    Remove-Item (Join-Path $t 'logs\tray.log')
    $gui = Start-Process $exe -PassThru
    Start-Sleep -Seconds 3
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Step 'GUI: stale value -> "registered from another folder" question' ($log -match 'registration: registered from another folder \(D:\\Old Folder\\' -and $log -match 'question box: VR EyeCapture is registered for OpenXR games from another folder:\s+D:\\Old Folder\\XR_APILAYER_JRDEVO_eye_capture\.json\s+Re-register it from this folder\? Windows will ask for administrator rights once\. -> No')
    Remove-Item Env:\EYECAP_TRAY_TEST_SHM; Remove-Item Env:\EYECAP_TRAY_TEST_REG
    $r = TrayCli $exe '--register'
    $ok = $false
    for ($i = 0; $i -lt 30 -and -not $ok; $i++) {
        Start-Sleep -Milliseconds 500
        $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
        $ok = $log -match 'tray: registration: registered \(' -and $log -match 'tooltip now: VR EyeCapture: no VR game running'
    }
    Step 'GUI: picks up an outside --register within 15 s (re-query), tooltip back to normal' $ok

    # ---- start_with_windows = 1 (the user's choice in config.ini): the tray restores ITS OWN Run value when it is missing or
    # points at another folder, never when the user switched it off in Task Manager (StartupApproved record odd); it never
    # writes StartupApproved (docs/DEVELOPMENT.md "Antivirus: behaviour rules")
    Stop-Process -Id $gui.Id; $gui = $null
    Start-Sleep -Milliseconds 500
    $cfgDir = Join-Path $t 'appdata\VR EyeCapture'
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $cfgDir 'config.ini'), "[capture]`r`nstart_with_windows = 1`r`n", (New-Object System.Text.UTF8Encoding $false))
    if (Test-Path $runKey) { Remove-Item -Path $runKey -Recurse -Force }
    Remove-Item -Path "$($runKey)Approved" -Recurse -Force -ErrorAction SilentlyContinue
    $want = '"' + $exe + '"'
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-install-test'
    foreach ($case in @(@{ name = 'missing'; before = $null }, @{ name = 'another folder'; before = '"X:\Moved\EyeCaptureTray.exe"' })) {
        if ($case.before) { New-Item -Path $runKey -Force | Out-Null; New-ItemProperty -Path $runKey -Name 'VR EyeCapture' -Value $case.before -PropertyType String -Force | Out-Null }
        Remove-Item (Join-Path $t 'logs\tray.log') -ErrorAction SilentlyContinue
        $gui = Start-Process $exe -PassThru
        Start-Sleep -Seconds 3
        $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
        $now = (Get-ItemProperty -Path $runKey -Name 'VR EyeCapture' -ErrorAction SilentlyContinue).'VR EyeCapture'
        Step "start_with_windows = 1, Run value $($case.name): restored to this exe, logged, no StartupApproved key" ($now -eq $want -and $log -match 'tray: start with Windows: start_with_windows = 1 and the Run value was .* -- restored to' -and -not (Test-Path "$($runKey)Approved"))
        Stop-Process -Id $gui.Id; $gui = $null
        Start-Sleep -Milliseconds 500
    }
    # switched off in Task Manager: the value (pointing elsewhere) and the record stay exactly as they are
    New-ItemProperty -Path $runKey -Name 'VR EyeCapture' -Value '"X:\Moved\EyeCaptureTray.exe"' -PropertyType String -Force | Out-Null
    New-Item -Path "$($runKey)Approved" -Force | Out-Null
    New-ItemProperty -Path "$($runKey)Approved" -Name 'VR EyeCapture' -Value ([byte[]](3,0,0,0,0,0,0,0,0,0,0,0)) -PropertyType Binary -Force | Out-Null
    $snap = RunSnapshot
    Remove-Item (Join-Path $t 'logs\tray.log') -ErrorAction SilentlyContinue
    $gui = Start-Process $exe -PassThru
    Start-Sleep -Seconds 3
    $log = Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8
    Step 'start_with_windows = 1 but switched off in Task Manager: Run value and record UNCHANGED, nothing restored' ((RunSnapshot) -eq $snap -and $log -notmatch 'restored to')
    Stop-Process -Id $gui.Id; $gui = $null
    Remove-Item -Path $runKey -Recurse -Force
    Remove-Item -Path "$($runKey)Approved" -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item Env:\EYECAP_TRAY_TEST_SHM

    # ---- 3c. installed copy: user data outside the program folder; the installer's --quit / --uninstall-cleanup
    if ($gui -and -not $gui.HasExited) { Stop-Process -Id $gui.Id; $gui = $null }
    $ti = Join-Path $root 'build\tt3i'
    Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$ti*" } | Stop-Process
    if (Test-Path $ti) { Remove-Item -Recurse -Force $ti }
    New-Item -ItemType Directory -Force $ti, (Join-Path $ti 'local'), (Join-Path $ti 'roam'), (Join-Path $ti 'videos') | Out-Null
    Copy-Item $trayBin $ti
    Copy-Item (Join-Path $root 'config.ini') $ti
    New-Item -ItemType File (Join-Path $ti $jsonName) | Out-Null
    New-Item -ItemType File (Join-Path $ti 'eye_capture.dll') | Out-Null
    Set-Content (Join-Path $ti 'installed.txt') 'test' -Encoding ascii
    $exeI = Join-Path $ti 'EyeCaptureTray.exe'
    $oldLocal = $env:LOCALAPPDATA
    $env:LOCALAPPDATA = Join-Path $ti 'local'; $env:APPDATA = Join-Path $ti 'roam'; $env:XR_EYE_CAPTURE_TEST_VIDEOS = Join-Path $ti 'videos'
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-install-test'
    try {
        $gui = Start-Process $exeI -PassThru
        Start-Sleep -Seconds 3
        $trayLog = Join-Path $ti 'local\VR EyeCapture\logs\tray.log'
        $log = if (Test-Path $trayLog) { Get-Content $trayLog -Raw -Encoding UTF8 } else { '' }
        Step 'installed copy: tray.log in %LOCALAPPDATA%\VR EyeCapture\logs, none next to the exe' ((Test-Path $trayLog) -and -not (Test-Path (Join-Path $ti 'logs')))
        Step 'installed copy: config.ini seeded to %APPDATA%\VR EyeCapture, logged' ((Test-Path (Join-Path $ti 'roam\VR EyeCapture\config.ini')) -and $log -match 'installed copy -- config\.ini copied from the program folder')
        Step 'installed copy: default recordings folder <Videos>\VR EyeCapture' ($log -match [regex]::Escape("recordings by default in $ti\videos\VR EyeCapture"))
        $r = TrayCli $exeI '--quit'
        Start-Sleep -Milliseconds 300
        Step '--quit: the running tray of this folder exits' ($r.code -eq 0 -and $r.out -match 'quit: 1 tray\(s\) of this folder asked to exit, 0 still running' -and $gui.HasExited)
        $gui = $null
        # what the uninstaller cleans: test Run value, test HKLM registration of this folder, test Home value
        New-Item -Path $runKey -Force | Out-Null
        New-ItemProperty -Path $runKey -Name 'VR EyeCapture' -Value ('"' + $exeI + '"') -PropertyType String -Force | Out-Null
        SetVal $lmKey (Join-Path $ti $jsonName)
        $homeKey = 'HKCU:\Software\OpenXR-EyeCapture-Test\Home'
        New-Item -Path $homeKey -Force | Out-Null
        New-ItemProperty -Path $homeKey -Name 'Home' -Value $ti -PropertyType String -Force | Out-Null
        $env:XR_EYE_CAPTURE_TEST_HOME_KEY = 'Software\OpenXR-EyeCapture-Test\Home'
        $r = TrayCli $exeI '--uninstall-cleanup'
        Remove-Item Env:\XR_EYE_CAPTURE_TEST_HOME_KEY
        $runLeft = (Get-Item $runKey -ErrorAction SilentlyContinue).GetValueNames() -contains 'VR EyeCapture'
        $homeLeft = (Test-Path $homeKey) -and ((Get-Item $homeKey).GetValueNames() -contains 'Home')
        Step '--uninstall-cleanup: exit 0; Run value, HKLM registration and Home value removed' ($r.code -eq 0 -and -not $runLeft -and (Ours $lmKey).Count -eq 0 -and -not $homeLeft -and $r.out -match 'OpenXR layer: unregistered' -and $r.out -match 'start with Windows: removed')
    } finally {
        $env:LOCALAPPDATA = $oldLocal
        Remove-Item Env:\XR_EYE_CAPTURE_TEST_VIDEOS -ErrorAction SilentlyContinue
        Remove-Item Env:\EYECAP_TRAY_TEST_SHM -ErrorAction SilentlyContinue
        Remove-Item -Path $runKey -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -Path 'HKCU:\Software\OpenXR-EyeCapture-Test\Home' -Recurse -Force -ErrorAction SilentlyContinue
    }
} finally {
    $env:APPDATA = $oldAppdata
    foreach ($v in @('EYECAP_TRAY_TEST_REG_ROOTS', 'EYECAP_TRAY_TEST_RUN_KEY', 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_REG')) { Remove-Item "Env:\$v" -ErrorAction SilentlyContinue }
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
