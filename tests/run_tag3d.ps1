<#
    run_tag3d.ps1 -- the tray's "Tag a video as 3D" (tray menu -> Side by side... / Top / bottom...), tray/postprocess.cpp Tag3d,
    through the CLI twin `EyeCaptureTray.exe --tag3d <sbs|tb> <file>`. CPU only, no game, does not touch the installed tray.
      powershell -ExecutionPolicy Bypass -File tests\run_tag3d.ps1 [-FfmpegDir <dir>]   (default: $env:EYECAP_FFMPEG_DIR, else WinGet packages, else PATH)
    A scratch copy of build\Release\EyeCaptureTray.exe runs in build\tt_tag3d (its own logs\tray.log). ffmpeg makes a 2 s
    1280x360 testsrc2 clip with AAC audio as .mp4 and .mkv (no stereo tags, like an OBS side-by-side recording), then:
    1. sbs on the .mp4  -> <name>_3D.mkv: video stream tag STEREO_MODE=left_right (Matroska StereoMode), codec/size/duration unchanged,
                           audio stream kept, the source untouched.
    2. tb on the .mkv   -> <name>_3D.mkv with STEREO_MODE=top_bottom.
    3. sbs on the .mp4 again: never overwrites -> "<name>_3D (2).mkv" (left_right), the first file untouched.
    4. no .part file is left behind; tray.log has the logged ffmpeg command and "done".
    5. a missing file and a bad mode fail (exit code 1 / 2, no output file).
#>
param([string]$FfmpegDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $FfmpegDir) { $FfmpegDir = $env:EYECAP_FFMPEG_DIR }
if (-not $FfmpegDir) {
    $c = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { $FfmpegDir = $c.DirectoryName }
}
$ffmpeg = if ($FfmpegDir) { Join-Path $FfmpegDir 'ffmpeg.exe' } else { (Get-Command ffmpeg.exe -ErrorAction Stop).Source }
$ffprobe = Join-Path (Split-Path $ffmpeg) 'ffprobe.exe'
$trayExe = Join-Path $root 'build\Release\EyeCaptureTray.exe'
if (-not (Test-Path $trayExe)) { throw "build EyeCaptureTray first: $trayExe" }

$work = Join-Path $root 'build\tt_tag3d'
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory $work | Out-Null
Copy-Item $trayExe $work
$tray = Join-Path $work 'EyeCaptureTray.exe'
# config.ini with ffmpeg_path pinned so the tray does not depend on PATH
Set-Content -Path (Join-Path $work 'config.ini') -Value "[capture]`r`nffmpeg_path = $ffmpeg`r`n" -Encoding ASCII

$fails = 0
function Check($ok, $what) {
    if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:fails++ }
}
function Probe($file, $entries, $sel) {
    # returns "key=value" lines of the selected stream
    & $ffprobe -v error -select_streams $sel -show_entries $entries -of default=nw=1 $file
}
function RunTray([string[]]$a) {
    $p = Start-Process -FilePath $tray -ArgumentList $a -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $work 'out.txt') -RedirectStandardError (Join-Path $work 'err.txt')
    [pscustomobject]@{ Code = $p.ExitCode; Out = (Get-Content (Join-Path $work 'out.txt') -Raw) }
}

$mp4 = Join-Path $work 'obs clip.mp4'      # a space in the name on purpose
$mkv = Join-Path $work 'other_clip.mkv'
& $ffmpeg -v error -y -f lavfi -i "testsrc2=size=1280x360:rate=30:duration=2" -f lavfi -i "sine=frequency=440:duration=2" -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest $mp4
& $ffmpeg -v error -y -i $mp4 -c copy $mkv
$srcDur = [double]((Probe $mp4 'format=duration' 'v:0' | Select-String 'duration=').ToString().Split('=')[1])
$srcHash = (Get-FileHash $mp4).Hash

Write-Host '1. side by side on an mp4'
$r = RunTray @('--tag3d', 'sbs', "`"$mp4`"")
$out1 = Join-Path $work 'obs clip_3D.mkv'
Check ($r.Code -eq 0) "exit code 0 (got $($r.Code), output: $($r.Out.Trim()))"
Check (Test-Path $out1) 'obs clip_3D.mkv exists'
$tags = (& $ffprobe -v error -select_streams v:0 -show_entries stream_tags=STEREO_MODE:stream=codec_name,width,height -of default=nw=1 $out1) -join ' '
Check ($tags -match 'STEREO_MODE=left_right') "video stream tag STEREO_MODE=left_right ($tags)"
Check ($tags -match 'codec_name=h264' -and $tags -match 'width=1280' -and $tags -match 'height=360') 'video copied unchanged (h264 1280x360)'
$d1 = [double]((& $ffprobe -v error -show_entries format=duration -of default=nw=1 $out1 | Select-String 'duration=').ToString().Split('=')[1])
Check ([math]::Abs($d1 - $srcDur) -lt 0.1) "duration equals the source ($d1 vs $srcDur)"
Check ((& $ffprobe -v error -select_streams a -show_entries stream=codec_name -of default=nw=1 $out1) -match 'aac') 'audio stream kept'
Check ((Get-FileHash $mp4).Hash -eq $srcHash) 'source untouched'

Write-Host '2. top / bottom on an mkv'
$r = RunTray @('--tag3d', 'tb', "`"$mkv`"")
$out2 = Join-Path $work 'other_clip_3D.mkv'
Check ($r.Code -eq 0 -and (Test-Path $out2)) 'exit code 0 and other_clip_3D.mkv exists'
$tags = (& $ffprobe -v error -select_streams v:0 -show_entries stream_tags=STEREO_MODE -of default=nw=1 $out2) -join ' '
Check ($tags -match 'STEREO_MODE=top_bottom') "STEREO_MODE=top_bottom ($tags)"

Write-Host '3. never overwrites'
$h1 = (Get-FileHash $out1).Hash
$r = RunTray @('--tag3d', 'sbs', "`"$mp4`"")
$out3 = Join-Path $work 'obs clip_3D (2).mkv'
Check ($r.Code -eq 0 -and (Test-Path $out3)) 'second run wrote "obs clip_3D (2).mkv"'
Check ((Get-FileHash $out1).Hash -eq $h1) 'the first _3D file is untouched'
$tags = (& $ffprobe -v error -select_streams v:0 -show_entries stream_tags=STEREO_MODE -of default=nw=1 $out3) -join ' '
Check ($tags -match 'STEREO_MODE=left_right') '(2) file tagged left_right'

Write-Host '4. no .part left, log lines'
Check (-not (Get-ChildItem $work -Filter '*.part')) 'no .part file'
$log = Get-Content (Join-Path $work 'logs\tray.log') -Raw
Check ($log -match 'tag3d: .*-map 0 -dn -c copy -metadata:s:v:0 stereo_mode=left_right') 'log has the ffmpeg command (left_right)'
Check ($log -match 'stereo_mode=top_bottom') 'log has the ffmpeg command (top_bottom)'
Check ($log -match 'tag3d: done ') 'log has "done"'

Write-Host '5. failures'
$r = RunTray @('--tag3d', 'sbs', "`"$(Join-Path $work 'nope.mp4')`"")
Check ($r.Code -eq 1 -and $r.Out -match 'error: file not found') "missing file: exit 1, '$($r.Out.Trim())'"
$r = RunTray @('--tag3d', 'diag', "`"$mp4`"")
Check ($r.Code -eq 2) "bad mode: exit 2 (got $($r.Code))"
Check ((Get-Content (Join-Path $work 'logs\tray.log') -Raw) -match 'tag3d: .*FAILED|tag3d: .*file not found') 'failure logged (LOGE)'

if ($fails) { Write-Host "$fails check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'tag3d: all checks passed' -ForegroundColor Green
