<#
    run_steamvr_ui.ps1 -- the tray's "SteamVR games" window and the automatic repair (tray/steamvr_ui.cpp) on a fake Steam
    install. CPU only: no GPU, no SteamVR, no real Steam or game folder, no real registry key.
      powershell -ExecutionPolicy Bypass -File tests\run_steamvr_ui.ps1
    1. steamvr_install_test.exe: the library behind the window (vdf / acf parsing on tests\fixtures\steam, discovery, scan,
       enable / disable / OpenComposite / repair / ini).
    2. A scratch tray (copy of EyeCaptureTray.exe in build\tt_svr, EYECAP_TRAY_TEST_SHM, scratch APPDATA) with the proxy in
       build\tt_svr\steamvr\openvr_api.dll (the zip layout). EYECAP_TRAY_TEST_STEAM_DIR = a fake Steam install
       (build\tt_svr\steam + a second library build\tt_svr\lib2, libraryfolders.vdf + appmanifest_*.acf); the real Steam folder
       is never read. XR_EYE_CAPTURE_TEST_HOME_KEY = a test HKCU key the tray must fill with its folder at start.
       EYECAP_TRAY_TEST_PICK names a file whose first line is the "picked" exe. settings_test.exe svr opens the window
       (WM_APP+9), waits for the background Steam scan, ticks / clicks / presses buttons and answers the REAL message boxes.
       Games (mock_openvr.dll plays Valve's openvr_api.dll, oc_shim_test.dll OpenComposite's):
         Fake Unity  Plugins\x86_64 + a 32-bit Plugins\x86 copy    Second VR  64-bit next to the exe (library 2)
         AC Game     EasyAntiCheat folder                          OC Game    OpenComposite + .steamvr-backup (library 2)
         Old Game    32-bit only (library 2)                       Flat Game  no openvr_api.dll (not listed)
         Exe Game    not a Steam game: "Add a game by exe..."
       Cases: the scan lists 5 games with their states (Off / OpenComposite installed / 32-bit, not supported) and caches
       them ([found] in steamvr_games.ini); Flat Game by exe -> "no openvr_api.dll found"; tick Fake Unity (32-bit copy
       skipped, untouched) -> On; a real mouse click on AC Game's box -> anti-cheat warning, No -> unchanged; Enable all ->
       Second VR on, AC / OC / Old skipped; tick OC Game -> restore SteamVR's DLL first -> On; Exe Game by exe -> On.
       Auto repair: game updates in Fake Unity and Second VR, Second VR's DLL held open ("running"); tray restart -> Fake Unity
       repaired + balloon, Second VR skipped, AC Game (never enabled) untouched; handle closed -> opening the window repairs
       Second VR; Exe Game updated while held open -> listed "Needs repair". Untick / Disable everything -> originals back;
       Rescan after Old Game was uninstalled -> gone.
    Only the scratch tray is stopped.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
$drv = Join-Path $bin 'settings_test.exe'
$proxy = Join-Path $bin 'openvr_api.dll'; $mock = Join-Path $bin 'mock_openvr.dll'; $oc = Join-Path $bin 'oc_shim_test.dll'
# the games' own openvr_api.dll: Valve's v2.15.6 (the mock has test exports the proxy lacks: Enable would refuse it)
$valveDll = Join-Path $root 'third_party\openvr\bin\win64\openvr_api.dll'
foreach ($f in @($drv, $proxy, $mock, $oc, (Join-Path $bin 'EyeCaptureTray.exe'), (Join-Path $bin 'steamvr_install_test.exe'))) {
    if (-not (Test-Path $f)) { throw "build first: $f" }
}
$fail = 0
function Step($name, $ok) { Write-Host ("{0}: {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' }); if (-not $ok) { $script:fail++ } }
function Hash($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { '' } }
$utf8 = New-Object System.Text.UTF8Encoding $false

# ---- 1. the library
& (Join-Path $bin 'steamvr_install_test.exe') | Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
Step 'steamvr_install_test' ($LASTEXITCODE -eq 0)

# ---- 2. the fake Steam install and the scratch tray
$t = Join-Path $root 'build\tt_svr'
Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
$appCfg = Join-Path $t 'appdata\VR EyeCapture'
$steam = Join-Path $t 'steam'; $lib2 = Join-Path $t 'lib2'
$c1 = Join-Path $steam 'steamapps\common'; $c2 = Join-Path $lib2 'steamapps\common'
New-Item -ItemType Directory -Force $appCfg, (Join-Path $t 'steamvr'), $c1, $c2 | Out-Null
Copy-Item (Join-Path $bin 'EyeCaptureTray.exe') $t
Copy-Item $proxy (Join-Path $t 'steamvr\openvr_api.dll')
[System.IO.File]::WriteAllText((Join-Path $appCfg 'config.ini'), "[capture]`r`nhotkey = ScrollLock`r`n[debug]`r`nlog_level = info`r`n", $utf8)
$hValve = Hash $valveDll; $hProxy = Hash $proxy; $hOc = Hash $oc
$esc = { param($p) $p -replace '\\', '\\' }
[System.IO.File]::WriteAllText((Join-Path $steam 'steamapps\libraryfolders.vdf'),
    "`"libraryfolders`"`n{`n`t`"0`"`n`t{`n`t`t`"path`"`t`t`"$(& $esc $steam)`"`n`t}`n`t`"1`"`n`t{`n`t`t`"path`"`t`t`"$(& $esc $lib2)`"`n`t}`n}`n", $utf8)
function Acf($lib, $id, $name, $dir) {
    [System.IO.File]::WriteAllText((Join-Path $lib "steamapps\appmanifest_$id.acf"), "`"AppState`"`n{`n`t`"appid`"`t`t`"$id`"`n`t`"name`"`t`t`"$name`"`n`t`"installdir`"`t`t`"$dir`"`n}`n", $utf8)
}
function Exe($dir, $name) { New-Item -ItemType Directory -Force $dir | Out-Null; Set-Content (Join-Path $dir "$name.exe") 'MZ fake' -Encoding ascii }
$bytes = [System.IO.File]::ReadAllBytes($valveDll); $lfa = [BitConverter]::ToInt32($bytes, 0x3c); $bytes[$lfa + 4] = 0x4c; $bytes[$lfa + 5] = 0x01
# Fake Unity (64 + 32), AC Game (anti-cheat), Flat Game (no DLL) in library 1
$u = Join-Path $c1 'Fake Unity'; $u64 = Join-Path $u 'Fake Unity_Data\Plugins\x86_64'; $u32 = Join-Path $u 'Fake Unity_Data\Plugins\x86'
Exe $u 'Fake Unity'; New-Item -ItemType Directory -Force $u64, $u32 | Out-Null
Copy-Item $valveDll (Join-Path $u64 'openvr_api.dll'); [System.IO.File]::WriteAllBytes((Join-Path $u32 'openvr_api.dll'), $bytes)
$h32 = Hash (Join-Path $u32 'openvr_api.dll')
$a = Join-Path $c1 'AC Game'; Exe $a 'AC Game'; New-Item -ItemType Directory -Force (Join-Path $a 'EasyAntiCheat') | Out-Null
Copy-Item $valveDll (Join-Path $a 'openvr_api.dll')
Exe (Join-Path $c1 'Flat Game') 'Flat Game'
Acf $steam 1001 'Fake Unity' 'Fake Unity'; Acf $steam 1002 'AC Game' 'AC Game'; Acf $steam 1003 'Flat Game' 'Flat Game'
# Second VR, OC Game, Old Game (32-bit only) in library 2
$s = Join-Path $c2 'Second VR'; Exe $s 'Second VR'; Copy-Item $valveDll (Join-Path $s 'openvr_api.dll')
$o = Join-Path $c2 'OC Game'; Exe $o 'OC Game'; Copy-Item $oc (Join-Path $o 'openvr_api.dll'); Copy-Item $valveDll (Join-Path $o 'openvr_api.dll.steamvr-backup')
$old = Join-Path $c2 'Old Game'; Exe $old 'Old Game'; [System.IO.File]::WriteAllBytes((Join-Path $old 'openvr_api.dll'), $bytes)
Acf $lib2 2001 'Second VR' 'Second VR'; Acf $lib2 2002 'OC Game' 'OC Game'; Acf $lib2 2003 'Old Game' 'Old Game'
# Exe Game: not a Steam game
$x = Join-Path $t 'other\Exe Game'; Exe $x 'Exe Game'; Copy-Item $valveDll (Join-Path $x 'openvr_api.dll')

$pick = Join-Path $t 'pick.txt'
$ini = Join-Path $t 'steamvr_games.ini'
$homeKey = "Software\OpenXR-EyeCapture-Test\SvrHome$PID"
$saved = @{}
foreach ($n in 'APPDATA', 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_PICK', 'XR_EYE_CAPTURE_TEST_HOME_KEY', 'EYECAP_TRAY_TEST_STEAM_DIR') { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
$p = $null
$hold = $null
function Svr([string[]]$a) {
    $out = & $drv svr $p.Id @a 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $out | ForEach-Object { Write-Host "    $_" }
    return @{ code = $code; text = ($out -join "`n"); rows = @($out | Where-Object { $_ -like 'ROW: *' }) }
}
function Row($r, $game) { $l = @($r.rows | Where-Object { $_ -like "ROW: $game |*" }); if ($l.Count) { $l[0] } else { '' } }
function PickExe($exe) { [System.IO.File]::WriteAllText($pick, "$exe`r`n", $utf8) }
function StartTray { $script:p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru; Start-Sleep -Seconds 2 }
function LogText { Get-Content (Join-Path $t 'logs\tray.log') -Raw -Encoding UTF8 }
try {
    $env:APPDATA = Join-Path $t 'appdata'
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-svr-test'
    $env:EYECAP_TRAY_TEST_PICK = $pick
    $env:XR_EYE_CAPTURE_TEST_HOME_KEY = $homeKey
    $env:EYECAP_TRAY_TEST_STEAM_DIR = $steam
    StartTray
    $homeVal = (Get-ItemProperty -Path "HKCU:\$homeKey" -Name Home -ErrorAction SilentlyContinue).Home
    Step 'tray start writes its folder to the (test) Home key' ($homeVal -eq $t)

    Write-Host '-- the Steam scan'
    $r = Svr @('list')
    Step 'scan: 5 SteamVR games in 2 libraries (Flat Game has no openvr_api.dll: not listed)' ($r.code -eq 0 -and $r.rows.Count -eq 5 -and $r.text -match 'STATUS: Steam library: 5 SteamVR game\(s\) in 2 libraries\.')
    Step 'scan: states Off / OpenComposite installed / 32-bit, not supported' ((Row $r 'Fake Unity') -match '\| Off \|' -and (Row $r 'AC Game') -match '\| Off \|' -and (Row $r 'Second VR') -match '\| Off \|' -and (Row $r 'OC Game') -match '\| OpenComposite installed \|' -and (Row $r 'Old Game') -match '\| 32-bit, not supported \|')
    Step 'scan: sorted by name, Enable all available, Disable greyed' ($r.rows[0] -like 'ROW: AC Game |*' -and $r.rows[4] -like 'ROW: Second VR |*' -and $r.text -match 'BUTTONS: enableall=1 enable=1 disable=0 reenable=0')
    $iniText = Get-Content $ini -Raw
    Step 'scan cached in steamvr_games.ini ([found] x5, no [game])' (([regex]::Matches($iniText, '(?m)^\[found\]')).Count -eq 5 -and $iniText -notmatch '(?m)^\[game\]' -and $iniText -match 'appid = 2002')

    Write-Host '-- Recording mode (steamvr_mode)'
    $cfgFile = Join-Path $appCfg 'config.ini'
    Step 'mode: per game shown first (no steamvr_mode in config.ini = default)' ($r.text -match 'MODE: 0')
    $r = Svr @('mode', 'mode=1')
    Start-Sleep -Milliseconds 1500
    $c = Get-Content $cfgFile -Raw
    Step 'mode: Quick mode -> [steamvr] steamvr_mode = quick added, other lines kept, the tray re-reads it' ($c -match '(?m)^\[steamvr\]\r?\nsteamvr_mode = quick\r?$' -and $c -match '(?m)^hotkey = ScrollLock\r?$' -and (LogText) -match 'tray: steamvr_mode = quick' -and $r.text -match 'STATUS: .*Quick mode: SteamVR''s mirror picture is recorded')
    $r = Svr @('mode', 'mode=0')
    Start-Sleep -Milliseconds 1500
    $c = Get-Content $cfgFile -Raw
    Step 'mode: back to Per game -> rewritten in place (one line), shown on reopen' ($c -match '(?m)^steamvr_mode = per_game\r?$' -and ([regex]::Matches($c, 'steamvr_mode')).Count -eq 1 -and (LogText) -match 'steamvr_mode quick -> per_game saved' -and (LogText) -match 'tray: steamvr_mode = per_game' -and (Svr @('list')).text -match 'MODE: 0')

    Write-Host '-- Flat Game by exe'
    PickExe (Join-Path $c1 'Flat Game\Flat Game.exe')
    $r = Svr @('enable', 'answers=OK')
    Step 'Add a game by exe, Flat Game: "no openvr_api.dll found", nothing added' ($r.text -match 'BOX: Flat Game: no openvr_api\.dll found in' -and $r.rows.Count -eq 5)

    Write-Host '-- Odd Game by exe (its openvr_api.dll has exports the proxy lacks)'
    $odd = Join-Path $t 'other\Odd Game'; Exe $odd 'Odd Game'; Copy-Item $mock (Join-Path $odd 'openvr_api.dll')
    PickExe (Join-Path $odd 'Odd Game.exe')
    $r = Svr @('enable', 'answers=OK')
    Step 'Odd Game: the plan says "exports MockOpenVrCounts ..., which VR EyeCapture''s proxy does not; not enabled", nothing changed' ($r.text -match 'BOX: Odd Game: nothing to enable\..*exports MockOpenVrCounts.*which VR EyeCapture''s proxy does not; not enabled' -and (Hash (Join-Path $odd 'openvr_api.dll')) -eq (Hash $mock) -and -not (Test-Path (Join-Path $odd 'openvr_api_real.dll')) -and $r.rows.Count -eq 5)

    Write-Host '-- tick Fake Unity'
    $r = Svr @('toggle', 'row=Fake Unity', 'answers=Yes,OK')
    Step 'tick Fake Unity: confirmation lists the 64-bit folder and the 32-bit one (skipped)' ($r.text -match 'BOX: Enable SteamVR recording for Fake Unity\?.*Plugins\\x86_64\|\s+will be enabled' -and $r.text -match 'Plugins\\x86\|\s+32-bit -- not supported, skipped')
    Step 'tick Fake Unity: result box names ScrollLock and the automatic repair' ($r.text -match 'BOX: SteamVR recording is enabled for Fake Unity\..*press ScrollLock to record.*repairs it at its next start')
    Step 'tick Fake Unity: proxy in place, Valve''s DLL kept as openvr_api_real.dll, 32-bit copy untouched, row On' ((Hash (Join-Path $u64 'openvr_api.dll')) -eq $hProxy -and (Hash (Join-Path $u64 'openvr_api_real.dll')) -eq $hValve -and (Hash (Join-Path $u32 'openvr_api.dll')) -eq $h32 -and (Row $r 'Fake Unity') -match '\| On \|')
    $side = Get-Content (Join-Path $u64 'eye_capture_proxy.ini') -Raw -ErrorAction SilentlyContinue
    Step 'tick Fake Unity: sidecar home = the tray folder; [game] with appid 1001' ($side -match [regex]::Escape("home = $t") -and (Get-Content $ini -Raw) -match '(?ms)^\[game\]\r?\nname = Fake Unity\r?\nexe = \r?\nroot = .*?appid = 1001')

    Write-Host '-- a real click on AC Game''s check box (anti-cheat, answered No)'
    $r = Svr @('click', 'row=AC Game', 'answers=No')
    Step 'click AC Game: the confirmation warns about anti-cheat' ($r.text -match 'BOX: Enable SteamVR recording for AC Game\?.*WARNING: anti-cheat found \(EasyAntiCheat\)')
    Step 'click AC Game: No -> unchanged, still Off' ((Hash (Join-Path $a 'openvr_api.dll')) -eq $hValve -and -not (Test-Path (Join-Path $a 'openvr_api_real.dll')) -and (Row $r 'AC Game') -match '\| Off \|')

    Write-Host '-- Enable all'
    $r = Svr @('enableall', 'answers=Yes,OK')
    Step 'Enable all: asks for Second VR only, lists AC (anti-cheat), OC and Old as skipped' ($r.text -match 'BOX: Enable SteamVR recording for these games\?\|\|  Second VR\|\|Skipped:\|  AC Game -- anti-cheat.*\|  OC Game -- OpenComposite.*\|  Old Game -- 32-bit')
    Step 'Enable all: Second VR On (proxy in place), AC Game still Off' ((Hash (Join-Path $s 'openvr_api.dll')) -eq $hProxy -and (Row $r 'Second VR') -match '\| On \|' -and (Row $r 'AC Game') -match '\| Off \|' -and $r.text -match 'BOX: SteamVR recording enabled for 1 of 1 game')

    Write-Host '-- tick OC Game (OpenComposite)'
    $r = Svr @('toggle', 'row=OC Game', 'answers=Yes,Yes,OK')
    Step 'tick OC Game: asks to restore SteamVR''s openvr_api.dll first (alternatives)' ($r.text -match 'BOX: OpenComposite is installed for OC Game in.*alternatives for the same game.*Restore SteamVR''s openvr_api\.dll here first\?')
    Step 'tick OC Game: restored + On: proxy, real = Valve''s, OpenComposite kept as .opencomposite' ((Hash (Join-Path $o 'openvr_api.dll')) -eq $hProxy -and (Hash (Join-Path $o 'openvr_api_real.dll')) -eq $hValve -and (Hash (Join-Path $o 'openvr_api.dll.opencomposite')) -eq $hOc -and (Row $r 'OC Game') -match '\| On \|')

    Write-Host '-- Exe Game by exe'
    PickExe (Join-Path $x 'Exe Game.exe')
    $r = Svr @('enable', 'answers=Yes,OK')
    Step 'Add a game by exe: Exe Game On (6 rows)' ((Hash (Join-Path $x 'openvr_api.dll')) -eq $hProxy -and (Row $r 'Exe Game') -match '\| On \|' -and $r.rows.Count -eq 6)

    Write-Host '-- automatic repair at tray start'
    Stop-Process -Id $p.Id; $p.WaitForExit(); Start-Sleep -Milliseconds 300
    Copy-Item -Force $valveDll (Join-Path $u64 'openvr_api.dll')      # game update: Valve's DLL over the proxy
    Copy-Item -Force $valveDll (Join-Path $s 'openvr_api.dll')
    $hold = [System.IO.File]::Open((Join-Path $s 'openvr_api.dll'), 'Open', 'Read', 'Read')   # Second VR "running"
    Remove-Item (Join-Path $t 'logs\tray.log')
    StartTray
    Start-Sleep -Seconds 1
    $log = LogText
    Step 'tray start: Fake Unity repaired (logged), proxy back, the updated Valve DLL is openvr_api_real.dll' ($log -match 'steamvr: repair \(tray start\): Fake Unity, .*x86_64: repaired after a game update' -and (Hash (Join-Path $u64 'openvr_api.dll')) -eq $hProxy -and (Hash (Join-Path $u64 'openvr_api_real.dll')) -eq $hValve)
    Step 'tray start: one balloon "SteamVR recording repaired for Fake Unity after a game update"' ($log -match 'tray: balloon \[SteamVR games\] SteamVR recording repaired for Fake Unity after a game update' -and ([regex]::Matches($log, 'balloon \[SteamVR games\]')).Count -eq 1)
    Step 'tray start: Second VR in use -> skipped (logged), unchanged' ($log -match 'steamvr: repair \(tray start\): Second VR, .*: skipped -- in use' -and (Hash (Join-Path $s 'openvr_api.dll')) -eq $hValve)
    Step 'tray start: AC Game (never enabled) untouched' ((Hash (Join-Path $a 'openvr_api.dll')) -eq $hValve -and -not (Test-Path (Join-Path $a 'openvr_api_real.dll')) -and $log -notmatch 'repair \(tray start\): AC Game')
    $hold.Dispose(); $hold = $null
    $r = Svr @('list')
    Step 'window opened: Second VR repaired now (status line + log), On' ($r.text -match 'STATUS: .*SteamVR recording repaired for Second VR after a game update' -and (Hash (Join-Path $s 'openvr_api.dll')) -eq $hProxy -and (Row $r 'Second VR') -match '\| On \|' -and (LogText) -match 'repair \(window opened\): Second VR')
    Copy-Item -Force $valveDll (Join-Path $x 'openvr_api.dll')
    $hold = [System.IO.File]::Open((Join-Path $x 'openvr_api.dll'), 'Open', 'Read', 'Read')
    $r = Svr @('list')
    Step 'updated while running: Exe Game listed "Needs repair"' ((Row $r 'Exe Game') -match '\| Needs repair \(game updated\)')
    $hold.Dispose(); $hold = $null

    Write-Host '-- untick / Disable'
    $r = Svr @('toggle', 'row=Fake Unity', 'answers=Yes,OK')
    Step 'untick Fake Unity: Valve''s DLL back, no real / sidecar, row Off' ((Hash (Join-Path $u64 'openvr_api.dll')) -eq $hValve -and -not (Test-Path (Join-Path $u64 'openvr_api_real.dll')) -and -not (Test-Path (Join-Path $u64 'eye_capture_proxy.ini')) -and (Row $r 'Fake Unity') -match '\| Off \|')
    $r = Svr @('reenable', 'row=Exe Game', 'answers=Yes,OK')
    Step 'Re-enable Exe Game (repaired meanwhile when the window opened): confirmation + On' ($r.text -match 'BOX: Re-enable SteamVR recording for Exe Game\?.*already enabled' -and (LogText) -match 'repair \(window opened\): Exe Game' -and (Row $r 'Exe Game') -match '\| On \|')
    $r = Svr @('disable', 'row=Exe Game', 'answers=Yes,OK')
    Step 'Disable Exe Game: original back, row gone (not a Steam game)' ((Hash (Join-Path $x 'openvr_api.dll')) -eq $hValve -and (Row $r 'Exe Game') -eq '' -and $r.rows.Count -eq 5)
    $null = Svr @('disable', 'row=Second VR', 'answers=Yes,OK')
    $r = Svr @('disable', 'row=OC Game', 'answers=Yes,OK')
    Step 'Disable Second VR + OC Game: originals back (.opencomposite kept), everything Off, no [game] left' ((Hash (Join-Path $s 'openvr_api.dll')) -eq $hValve -and (Hash (Join-Path $o 'openvr_api.dll')) -eq $hValve -and (Hash (Join-Path $o 'openvr_api.dll.opencomposite')) -eq $hOc -and (Row $r 'OC Game') -match '\| Off \|' -and (Get-Content $ini -Raw) -notmatch '(?m)^\[game\]')

    Write-Host '-- Rescan after Old Game was uninstalled'
    Remove-Item -Recurse -Force $old; Remove-Item (Join-Path $lib2 'steamapps\appmanifest_2003.acf')
    $r = Svr @('rescan')
    Step 'Rescan: Old Game gone (4 rows)' ($r.rows.Count -eq 4 -and (Row $r 'Old Game') -eq '' -and $r.text -match 'STATUS: Steam library: 4 SteamVR game')

    $log = LogText
    Write-Host '--- tray.log (steamvr lines):'
    $log -split "`r?`n" | Where-Object { $_ -match 'steamvr: (enable|re-enable|disable|repair|Steam library|check box)|SteamVR home|balloon' } | ForEach-Object { Write-Host "  $_" }
    Step 'tray.log: proxy source = steamvr\openvr_api.dll, scans, boxes' ($log -match [regex]::Escape("(proxy $t\steamvr\openvr_api.dll") -and $log -match 'steamvr: Steam library scan done' -and $log -match 'steamvr: box: [\s\S]* -> Yes')
} finally {
    if ($hold) { $hold.Dispose() }
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
    Remove-Item -Path "HKCU:\$homeKey" -Recurse -ErrorAction SilentlyContinue
    $base = 'HKCU:\Software\OpenXR-EyeCapture-Test'
    if ((Test-Path $base) -and -not (Get-ChildItem $base) -and -not (Get-Item $base).ValueCount) { Remove-Item $base }
}
if ($fail) { Write-Host "run_steamvr_ui: $fail FAILED" -ForegroundColor Red; exit 1 } else { Write-Host 'run_steamvr_ui: all passed' -ForegroundColor Green; exit 0 }
