<#
    run_yt8k.ps1 -- the tray's "8K copy for YouTube" step (post_yt8k, tray/postprocess.cpp) end to end, CPU only.
      powershell -ExecutionPolicy Bypass -File tests\run_yt8k.ps1 [-FfmpegDir <dir>]
    A scratch tray (copy of EyeCaptureTray.exe in build\tt4, EYECAP_TRAY_TEST_SHM, scratch APPDATA config) converts
    synthetic recordings that status_sim announces as "saved <file>". The tray runs with EYECAP_TRAY_TEST_NO_NVENC=1
    (libx265, the GPU is never used) and EYECAP_TRAY_TEST_YT8K_SIZE=1536x864 (each eye in a 768x864 box instead of
    3840x4320, so x265 stays fast). Does not touch the installed tray, config.ini or the real status block.
    A. 1280x360 SBS (testsrc2 left, solid blue right; 640x360 wide eyes), 30 fps, 2 s, AAC; post_mkv 1, post_yt8k 1,
       keep_mp4 0: <name>.mkv + <name>_YT8K.mkv, the MP4 deleted. _YT8K.mkv: hevc 1536x864 yuv420p, stereo_mode
       left_right, ~2 s, 30 fps, audio kept; each eye 768x432 centred in its box (black bars above / below); the left
       box shows the left eye's colours, the right box the right eye's. Balloon "Ready for YouTube: ..." + "YouTube 8K: ...".
    B. 960x720 SBS (480x720 TALL eyes: testsrc2 left, solid orange right), 72 fps, 2 s; post_mkv 0, post_yt8k 1: only
       <name>_YT8K.mkv, 60 fps (r_frame_rate 60/1, ~120 frames), log "capped to 60 fps", MP4 kept. Each eye 576x864 with
       96 px black bars on BOTH sides of EACH eye (per-eye pad: the centre seam x 672..864 is black; a whole-frame pad
       would put the eyes side by side in the middle). Balloon "Ready for YouTube (8K): ...".
    C. a _VR180.mp4 with its vr180 sidecar; post_mkv 0, post_yt8k 1: "8K step skipped: VR180 ..." logged, no _YT8K.mkv,
       balloon "Nothing to convert".
#>
param([string]$FfmpegDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
foreach ($f in @('EyeCaptureTray.exe', 'status_sim.exe')) { if (-not (Test-Path (Join-Path $bin $f))) { throw "build first: $f" } }
if (-not $FfmpegDir) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffprobe.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cand) { $FfmpegDir = $cand.DirectoryName } else {
        $onPath = Get-Command ffprobe -ErrorAction SilentlyContinue
        if ($onPath) { $FfmpegDir = Split-Path $onPath.Source } else { throw 'ffprobe not found (pass -FfmpegDir)' }
    }
}
$ffmpeg = Join-Path $FfmpegDir 'ffmpeg.exe'
$ffprobe = Join-Path $FfmpegDir 'ffprobe.exe'
$fail = 0
function Step($name, $ok) { Write-Host ("{0}: {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' }); if (-not $ok) { $script:fail++ } }

$t = Join-Path $root 'build\tt4'
$c = Join-Path $t 'c'   # short path: status_sim's "saved <path>" message must stay under 160 characters
Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory (Join-Path $t 'appdata\OpenXR-EyeCapture'), $c | Out-Null
Copy-Item (Join-Path $bin 'EyeCaptureTray.exe') $t
$ini = Join-Path $t 'appdata\OpenXR-EyeCapture\config.ini'
$baseIni = Get-Content (Join-Path $root 'config.ini') -Raw
function WriteConfig($mkv, $yt8k, $keep) {
    $s = $baseIni -replace '(?m)^post_mkv = \S+', "post_mkv = $mkv" -replace '(?m)^post_flat = \S+', 'post_flat = 0' `
                  -replace '(?m)^post_yt8k = \S+', "post_yt8k = $yt8k" -replace '(?m)^keep_mp4 = \S+', "keep_mp4 = $keep" `
                  -replace '(?m)^ffmpeg_path =.*$', "ffmpeg_path = $ffmpeg" -replace '(?m)^log_level = \S+', 'log_level = info'
    $s | Set-Content $ini -NoNewline -Encoding utf8
}

# ---- synthetic recordings (CPU, tiny)
$a = Join-Path $c 'ya_SBS.mp4'; $b = Join-Path $c 'yb_SBS.mp4'; $v = Join-Path $c 'yc_VR180.mp4'
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=640x360:rate=30:duration=2' -f lavfi -i 'color=c=0x1E3CC8:size=640x360:rate=30:duration=2' `
    -f lavfi -i 'sine=frequency=440:duration=2' -filter_complex '[0:v][1:v]hstack=inputs=2,format=yuv420p[v]' -map '[v]' -map 2:a `
    -c:v libx264 -preset veryfast -crf 16 -c:a aac -shortest $a
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=480x720:rate=72:duration=2' -f lavfi -i 'color=c=0xC83C1E:size=480x720:rate=72:duration=2' `
    -filter_complex '[0:v][1:v]hstack=inputs=2,format=yuv420p[v]' -map '[v]' -c:v libx264 -preset veryfast -crf 16 $b
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=1280x640:rate=30:duration=1' -c:v libx264 -preset veryfast -crf 20 $v
'{"projection":"vr180","layout":"left_right"}' | Set-Content ([IO.Path]::ChangeExtension($v, '.json')) -NoNewline -Encoding ascii
Step 'synthetic sources made' ((Test-Path $a) -and (Test-Path $b) -and (Test-Path $v))

function Probe($file, $entries, $extra = @()) {
    $o = & $ffprobe -v error -select_streams v:0 @extra -show_entries $entries -of default=nw=1 $file
    $h = @{}; foreach ($l in $o) { if ($l -match '^([^=]+)=(.*)$') { $h[$Matches[1]] = $Matches[2] } }; return $h
}
# average R,G,B of a region (crop w:h:x:y) of the frame at time $at: area-scaled to 32x16, then averaged here
function Avg($file, $crop, $at = 1.0) {
    $raw = Join-Path $t 'px.raw'
    & $ffmpeg -hide_banner -loglevel error -y -ss $at -i $file -frames:v 1 -vf "crop=$crop,scale=32:16:flags=area,format=rgb24" -f rawvideo $raw
    $px = [IO.File]::ReadAllBytes($raw); $s = @(0, 0, 0)
    for ($i = 0; $i -lt $px.Length; $i += 3) { $s[0] += $px[$i]; $s[1] += $px[$i + 1]; $s[2] += $px[$i + 2] }
    $n = $px.Length / 3; return @([math]::Round($s[0] / $n, 1), [math]::Round($s[1] / $n, 1), [math]::Round($s[2] / $n, 1))
}
function Near($p, $q, $tol) { for ($i = 0; $i -lt 3; $i++) { if ([math]::Abs($p[$i] - $q[$i]) -gt $tol) { return $false } }; return $true }
function Dist($p, $q) { return [math]::Abs($p[0] - $q[0]) + [math]::Abs($p[1] - $q[1]) + [math]::Abs($p[2] - $q[2]) }
function Fmt($p) { return '({0}, {1}, {2})' -f $p[0], $p[1], $p[2] }

$log = Join-Path $t 'logs\tray.log'
function LogText { if (Test-Path $log) { return (Get-Content $log -Raw -Encoding UTF8) } else { return '' } }
function JobsEnded { return ([regex]::Matches((LogText), 'job: (done|FAILED) ')).Count }
# announce one file with status_sim and wait until the tray's job for it ends
function RunJob($file) {
    $before = JobsEnded
    & (Join-Path $bin 'status_sim.exe') 'eyecap-yt8k-test' 1 $file | Out-Null
    $deadline = (Get-Date).AddSeconds(120)
    while ((JobsEnded) -le $before -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    Start-Sleep -Milliseconds 1000   # the balloon line follows the job's last line
    return ((JobsEnded) -gt $before)
}

$oldAppdata = $env:APPDATA
$p = $null
try {
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-yt8k-test'; $env:EYECAP_TRAY_TEST_NO_NVENC = '1'; $env:EYECAP_TRAY_TEST_YT8K_SIZE = '1536x864'
    $env:APPDATA = Join-Path $t 'appdata'
    WriteConfig 1 1 0
    $p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_NO_NVENC', 'EYECAP_TRAY_TEST_YT8K_SIZE') { Remove-Item "Env:\$n" }
    $env:APPDATA = $oldAppdata
    Start-Sleep -Seconds 2

    # ---- A: wide eyes, 30 fps, MKV + 8K, MP4 deleted
    Write-Host "`n=== A: 1280x360 SBS, 30 fps, post_mkv 1 + post_yt8k 1, keep_mp4 0" -ForegroundColor Cyan
    $srcL = Avg $a '640:360:0:0'; $srcR = Avg $a '640:360:640:0'
    Step 'A: job ended' (RunJob $a)
    $y = Join-Path $c 'ya_SBS_YT8K.mkv'
    Step 'A: _YT8K.mkv and .mkv exist, MP4 deleted (keep_mp4 0)' ((Test-Path $y) -and (Test-Path (Join-Path $c 'ya_SBS.mkv')) -and -not (Test-Path $a))
    if (Test-Path $y) {
        $s = Probe $y 'stream=codec_name,width,height,pix_fmt,r_frame_rate:stream_tags=stereo_mode:format=duration'
        $s.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Host ("  {0} = {1}" -f $_.Name, $_.Value) }
        $sd = (& $ffprobe -v error -select_streams v:0 -show_streams $y) -join "`n"
        Step 'A: hevc 1536x864 yuv420p 30/1' ($s.codec_name -eq 'hevc' -and $s.width -eq '1536' -and $s.height -eq '864' -and $s.pix_fmt -eq 'yuv420p' -and $s.r_frame_rate -eq '30/1')
        Step 'A: stereo_mode left_right (tag + Stereo 3D side data)' ($s.'TAG:stereo_mode' -eq 'left_right' -and $sd -match 'side by side')
        Step ('A: duration ~2 s ({0})' -f $s.duration) ([double]$s.duration -gt 1.9 -and [double]$s.duration -lt 2.15)
        $aud = & $ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 $y
        Step "A: audio copied ($aud)" ($aud -eq 'aac')
        # 640x360 eye -> 768x432 in a 768x864 box: content at y 216..648 in each half, black above / below
        $oL = Avg $y '752:416:8:224'; $oR = Avg $y '752:416:776:224'; $top = Avg $y '1536:200:0:0'; $bot = Avg $y '1536:200:0:664'
        Write-Host ("  source left {0} right {1} | 8K left box {2} right box {3} | bars top {4} bottom {5}" -f (Fmt $srcL), (Fmt $srcR), (Fmt $oL), (Fmt $oR), (Fmt $top), (Fmt $bot))
        Step 'A: left box = left eye, right box = right eye (avg colour within 10)' ((Near $oL $srcL 10) -and (Near $oR $srcR 10))
        Step 'A: the eyes differ (left vs right box avg distance > 60)' ((Dist $oL $oR) -gt 60)
        Step 'A: black bars above and below each eye' ((Near $top @(0, 0, 0) 6) -and (Near $bot @(0, 0, 0) 6))
    }
    $l = LogText
    Step 'A: log has the 8K libx265 command with the per-eye filter graph' ($l -match 'job: 8K cmd \(libx265\): .*split=2\[l\]\[r\];\[l\]crop=iw/2:ih:0:0,scale=768:864:force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,pad=768:864:-1:-1:color=black\[L\];\[r\]crop=iw/2:ih:iw/2:0,.*\[L\]\[R\]hstack=inputs=2,format=yuv420p.* -metadata:s:v:0 stereo_mode=left_right .*ya_SBS_YT8K\.mkv')
    Step 'A: log: test size + 30 fps kept + NO_NVENC' ($l -match 'EYECAP_TRAY_TEST_YT8K_SIZE=1536x864 \(test\)' -and $l -match 'source r_frame_rate 30/1 .* <= 60 -- frame rate kept' -and $l -match 'EYECAP_TRAY_TEST_NO_NVENC is set')
    Step 'A: balloon names the MKV and the 8K file' ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube: ya_SBS\.mkv\s+YouTube 8K: ya_SBS_YT8K\.mkv')

    # ---- B: tall eyes, 72 fps -> 60, 8K only
    Write-Host "`n=== B: 960x720 SBS (tall eyes), 72 fps, post_mkv 0 + post_yt8k 1, keep_mp4 0" -ForegroundColor Cyan
    WriteConfig 0 1 0
    $srcL = Avg $b '480:720:0:0'; $srcR = Avg $b '480:720:480:0'
    Step 'B: job ended' (RunJob $b)
    $y = Join-Path $c 'yb_SBS_YT8K.mkv'
    Step 'B: only _YT8K.mkv made, no .mkv, MP4 kept (no MKV step)' ((Test-Path $y) -and -not (Test-Path (Join-Path $c 'yb_SBS.mkv')) -and (Test-Path $b))
    if (Test-Path $y) {
        $s = Probe $y 'stream=width,height,r_frame_rate,nb_read_frames:stream_tags=stereo_mode:format=duration' @('-count_frames')
        $s.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Host ("  {0} = {1}" -f $_.Name, $_.Value) }
        Step ('B: 1536x864, 60/1, {0} frames (~120)' -f $s.nb_read_frames) ($s.width -eq '1536' -and $s.height -eq '864' -and $s.r_frame_rate -eq '60/1' -and [int]$s.nb_read_frames -ge 118 -and [int]$s.nb_read_frames -le 122)
        Step 'B: stereo_mode left_right' ($s.'TAG:stereo_mode' -eq 'left_right')
        # 480x720 eye -> 576x864, centred in its 768x864 box: 96 px black on both sides of EACH eye
        $oL = Avg $y '560:848:104:8'; $oR = Avg $y '560:848:872:8'
        $outerL = Avg $y '88:864:0:0'; $seam = Avg $y '176:864:680:0'; $outerR = Avg $y '88:864:1448:0'
        Write-Host ("  source left {0} right {1} | 8K left eye {2} right eye {3} | bars outer-left {4} seam {5} outer-right {6}" -f (Fmt $srcL), (Fmt $srcR), (Fmt $oL), (Fmt $oR), (Fmt $outerL), (Fmt $seam), (Fmt $outerR))
        Step 'B: left box = left eye, right box = right eye' ((Near $oL $srcL 10) -and (Near $oR $srcR 10) -and (Dist $oL $oR) -gt 60)
        Step 'B: per-eye pad: black outer bars AND a black centre seam (x 680..856)' ((Near $outerL @(0, 0, 0) 6) -and (Near $seam @(0, 0, 0) 6) -and (Near $outerR @(0, 0, 0) 6))
    }
    $l = LogText
    Step 'B: log: 72 fps source capped to 60' ($l -match 'source r_frame_rate 72/1 \(72\.000 fps, avg 72/1\) > 60 -- capped to 60 fps' -and $l -match 'job: 8K cmd \(libx265\): .*-vf "fps=60,split=2.*yb_SBS_YT8K\.mkv')
    Step 'B: balloon "Ready for YouTube (8K)"' ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube \(8K\): yb_SBS_YT8K\.mkv')

    # ---- C: dome recording -> skipped
    Write-Host "`n=== C: _VR180.mp4 + vr180 sidecar, post_mkv 0 + post_yt8k 1" -ForegroundColor Cyan
    Step 'C: job ended' (RunJob $v)
    $l = LogText
    Step 'C: skipped, logged, no _YT8K.mkv, MP4 kept' ($l -match 'job: 8K step skipped: VR180 recordings are already full-frame' -and -not (Test-Path (Join-Path $c 'yc_VR180_YT8K.mkv')) -and (Test-Path $v))
    Step 'C: balloon "Nothing to convert"' ($l -match 'balloon \[Nothing to convert\] yc_VR180\.mp4\s+VR180 recordings are already full-frame')

    Write-Host "`n--- tray.log (job lines):"
    (LogText) -split "`r?`n" | Where-Object { $_ -match 'job: |balloon' } | ForEach-Object { Write-Host "  $_" }
    Step 'no job FAILED' (-not ((LogText) -match 'job: FAILED'))
} finally {
    $env:APPDATA = $oldAppdata
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_NO_NVENC', 'EYECAP_TRAY_TEST_YT8K_SIZE') { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
}
if ($fail) { Write-Host "`n$fail check(s) FAILED" -ForegroundColor Red; exit 1 } else { Write-Host "`nall checks passed" -ForegroundColor Green; exit 0 }
