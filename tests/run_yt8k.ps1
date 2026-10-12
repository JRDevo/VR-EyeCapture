<#
    run_yt8k.ps1 -- the tray's "8K copy for YouTube" step (post_yt8k) and the layout_3d handling of the MKV step
    (post_mkv), tray/postprocess.cpp, end to end, CPU only.
      powershell -ExecutionPolicy Bypass -File tests\run_yt8k.ps1 [-FfmpegDir <dir>]   (default: $env:EYECAP_FFMPEG_DIR, else WinGet packages, else PATH)
    A scratch tray (copy of EyeCaptureTray.exe in build\tt4, EYECAP_TRAY_TEST_SHM, scratch APPDATA config) converts
    synthetic recordings that status_sim announces as "saved <file>". The tray runs with EYECAP_TRAY_TEST_NO_NVENC=1
    (libx265, the GPU is never used) and EYECAP_TRAY_TEST_YT8K_SHORT=432 (short side instead of
    4320, so x265 stays fast). Does not touch the installed tray, config.ini or the real status block.
    A. 1280x360 SBS (testsrc2 left, solid blue right; 640x360 wide eyes), 30 fps, 2 s, AAC; post_mkv 1, post_yt8k 1,
       keep_mp4 0: <name>.mkv + <name>_YT8K.mkv, the MP4 deleted. _YT8K.mkv is HALF top-bottom (YouTube reads 3D as
       half-format: the frame aspect is ONE eye's aspect): hevc 768x432 yuv420p (short side 432 = the test override), the
       stereo_mode=top_bottom tag, ~2 s, 30 fps, audio kept; the top half shows the left eye's colours, the bottom half
       the right eye's, no black bars. Balloon "Ready for YouTube: ..." + "YouTube 8K: ...".
    B. 960x720 SBS (480x720 TALL eyes: testsrc2 left, solid orange right), 72 fps, 2 s; short side 432 too (0.9x); post_mkv 0,
       post_yt8k 1: only <name>_YT8K.mkv, 432x648 (each eye 432x324), 60 fps (r_frame_rate 60/1, ~120 frames), log
       "capped to 60 fps", MP4 kept; top = left eye, bottom = right eye. Balloon "Ready for YouTube (8K): ...".
    D-G. layout_3d on copies of the case-A source (S = 432, 8K only, MP4 kept; the tray re-reads config.ini per job):
       sbs_half 768x432 left_right; sbs_full 1536x432 left_right; tb_full 432x~488 top_bottom; bogus -> tb_half 768x432 +
       "bad value" warning. Each: tag, left eye in the first (left / top) half and the blue eye in the second, logged
       layout line and -vf (hstack / vstack, scale divisors).
    H/I. layout_3d in the MKV step (post_mkv 1, post_yt8k 0, keep_mp4 0, copies of the case-A source, NO short-side override
       for this step: the MKV is at the native size): H = tb_half: <name>.mkv is a RE-ENCODE, hevc 640x360 (= one eye's
       size), 30 fps, top_bottom tag, top = left eye colour, bottom = the blue eye, the MP4 deleted only after the logged
       "MKV verification (re-encode): exists, N bytes > 0, duration .. vs MP4 .." line, no fps cap in the graph.
       I = sbs_full: <name>.mkv is the plain remux (stream copy: h264 1280x360, left_right tag, logged "MKV verification
       (remux)" size line), MP4 deleted.
    C. a _VR180.mp4 with its vr180 sidecar; post_mkv 0, post_yt8k 1: "8K step skipped: VR180 ..." logged, no _YT8K.mkv,
       balloon "Nothing to convert".
#>
param([string]$FfmpegDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
foreach ($f in @('EyeCaptureTray.exe', 'status_sim.exe')) { if (-not (Test-Path (Join-Path $bin $f))) { throw "build first: $f" } }
if (-not $FfmpegDir -and $env:EYECAP_FFMPEG_DIR) { $FfmpegDir = $env:EYECAP_FFMPEG_DIR }   # CI: dir with ffmpeg.exe + ffprobe.exe
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
function WriteConfig($mkv, $yt8k, $keep, $layout = 'tb_half', $crop = 'none') {
    # the tray re-reads config.ini for every job, so the layout can change between cases
    $s = $baseIni -replace '(?m)^post_mkv = \S+', "post_mkv = $mkv" -replace '(?m)^post_flat = \S+', 'post_flat = 0' `
                  -replace '(?m)^post_yt8k = \S+', "post_yt8k = $yt8k" -replace '(?m)^keep_mp4 = \S+', "keep_mp4 = $keep" `
                  -replace '(?m)^layout_3d = \S+', "layout_3d = $layout" -replace '(?m)^eye_crop = \S+', "eye_crop = $crop" `
                  -replace '(?m)^post_when = \S+', 'post_when = now' `
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
# layout cases D..G use copies of the case-A source (case A deletes its MP4)
$ld = @{}
foreach ($k in 'sbs_half', 'sbs_full', 'tb_full', 'bogus') { $ld[$k] = Join-Path $c ("yl{0}_SBS.mp4" -f $k.Replace('_', '')); Copy-Item $a $ld[$k] }
# eye_crop cases J..M: TALL eyes (640x900 each, 1280x900 SBS), solid orange right eye, copies per case
$tall = Join-Path $c 'yt_SBS.mp4'
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=640x900:rate=30:duration=2' -f lavfi -i 'color=c=0xC83C1E:size=640x900:rate=30:duration=2' `
    -filter_complex '[0:v][1:v]hstack=inputs=2,format=yuv420p[v]' -map '[v]' -c:v libx264 -preset veryfast -crf 16 $tall
$tc = @{}
foreach ($k in 'c169', 'c43', 'cnone', 'cmkv') { $tc[$k] = Join-Path $c ("yt{0}_SBS.mp4" -f $k); Copy-Item $tall $tc[$k] }
Step 'tall-eye sources made' ((Test-Path $tall) -and (Test-Path $tc['cmkv']))
# MKV-step cases H / I (layout_3d tb_half re-encode / sbs_full remux)
$mh = Join-Path $c 'yh_SBS.mp4'; $mi = Join-Path $c 'yi_SBS.mp4'; Copy-Item $a $mh; Copy-Item $a $mi

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
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-yt8k-test'; $env:EYECAP_TRAY_TEST_NO_NVENC = '1'; $env:EYECAP_TRAY_TEST_YT8K_SHORT = '432'
    $env:APPDATA = Join-Path $t 'appdata'
    WriteConfig 1 1 0
    $p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_NO_NVENC', 'EYECAP_TRAY_TEST_YT8K_SHORT') { Remove-Item "Env:\$n" }
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
        Step 'A: hevc 768x432 yuv420p 30/1' ($s.codec_name -eq 'hevc' -and $s.width -eq '768' -and $s.height -eq '432' -and $s.pix_fmt -eq 'yuv420p' -and $s.r_frame_rate -eq '30/1')
        Step 'A: stereo_mode top_bottom (tag)' ($s.'TAG:stereo_mode' -eq 'top_bottom')
        Write-Host ("  Stereo 3D side data: {0}" -f $(if ($sd -match 'top and bottom') { 'top and bottom' } elseif ($sd -match 'side by side') { 'SIDE BY SIDE' } else { 'none reported' }))
        Step ('A: duration ~2 s ({0})' -f $s.duration) ([double]$s.duration -gt 1.9 -and [double]$s.duration -lt 2.15)
        $aud = & $ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 $y
        Step "A: audio copied ($aud)" ($aud -eq 'aac')
        # 640x360 eye -> 768x432 display, stored 768x216 per eye: top half = left eye, bottom half = right eye
        $oL = Avg $y '768:216:0:0'; $oR = Avg $y '768:216:0:216'; $edT = Avg $y '768:6:0:0'; $edB = Avg $y '768:6:0:426'; $edL = Avg $y '6:432:0:0'; $edR = Avg $y '6:432:762:0'
        Write-Host ("  source left {0} right {1} | 8K top {2} bottom {3} | edges top {4} bottom {5} left {6} right {7}" -f (Fmt $srcL), (Fmt $srcR), (Fmt $oL), (Fmt $oR), (Fmt $edT), (Fmt $edB), (Fmt $edL), (Fmt $edR))
        Step 'A: top half = left eye, bottom half = right eye (avg colour within 10)' ((Near $oL $srcL 10) -and (Near $oR $srcR 10))
        Step 'A: the eyes differ (top vs bottom avg distance > 60)' ((Dist $oL $oR) -gt 60)
        Step 'A: no black bars (no frame edge is black)' (-not (Near $edT @(0, 0, 0) 12) -and -not (Near $edB @(0, 0, 0) 12) -and -not (Near $edL @(0, 0, 0) 12) -and -not (Near $edR @(0, 0, 0) 12))
    }
    $l = LogText
    Step 'A: log has the 8K libx265 command with the vstack expression graph' ($l -match 'job: 8K cmd \(libx265\): .*split=2\[l\]\[r\];\[l\]crop=iw/2:ih:0:0\,scale=w=2\*round\(iw\*min\(432/min\(iw\\,ih\)\\,8192/max\(iw\\,ih\)\)/2\):h=2\*round\(ih\*min\(.*\)/4\):flags=lanczos\[L\];\[r\]crop=iw/2:ih:iw/2:0\,.*\[L\]\[R\]vstack=inputs=2\,setsar=1\,format=yuv420p.* -metadata:s:v:0 stereo_mode=top_bottom .*ya_SBS_YT8K\.mkv')
    Step 'A: log: layout + test short side + 30 fps kept + NO_NVENC' ($l -match 'layout_3d tb_half: frame = one eye''s aspect, each eye squeezed to half height, left eye on top; frame short side 432' -and $l -match 'EYECAP_TRAY_TEST_YT8K_SHORT=432 \(test\)' -and $l -match 'source r_frame_rate 30/1 .* <= 60 -- frame rate kept' -and $l -match 'EYECAP_TRAY_TEST_NO_NVENC is set')
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
        Step ('B: 432x648, 60/1, {0} frames (~120)' -f $s.nb_read_frames) ($s.width -eq '432' -and $s.height -eq '648' -and $s.r_frame_rate -eq '60/1' -and [int]$s.nb_read_frames -ge 118 -and [int]$s.nb_read_frames -le 122)
        Step 'B: stereo_mode top_bottom' ($s.'TAG:stereo_mode' -eq 'top_bottom')
        # 480x720 eye -> 432x648 display, stored 432x324 per eye
        $oL = Avg $y '432:324:0:0'; $oR = Avg $y '432:324:0:324'; $edL = Avg $y '6:324:0:0'; $edR = Avg $y '6:648:426:0'
        Write-Host ("  source left {0} right {1} | 8K top {2} bottom {3} | edges left {4} right {5}" -f (Fmt $srcL), (Fmt $srcR), (Fmt $oL), (Fmt $oR), (Fmt $edL), (Fmt $edR))
        Step 'B: top half = left eye, bottom half = right eye' ((Near $oL $srcL 10) -and (Near $oR $srcR 10) -and (Dist $oL $oR) -gt 60)
        Step 'B: no black bars (no frame edge is black)' (-not (Near $edL @(0, 0, 0) 12) -and -not (Near $edR @(0, 0, 0) 12))
    }
    $l = LogText
    Step 'B: log: 72 fps source capped to 60' ($l -match 'source r_frame_rate 72/1 \(72\.000 fps, avg 72/1\) > 60 -- capped to 60 fps' -and $l -match 'job: 8K cmd \(libx265\): .*-vf "fps=60,split=2.*yb_SBS_YT8K\.mkv')
    Step 'B: balloon "Ready for YouTube (8K)"' ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube \(8K\): yb_SBS_YT8K\.mkv')

    # ---- D..G: the other layouts on the case-A source (640x360 eyes, S = 432), 8K only, MP4 kept
    # $key = file key, $cfgVal = value written to config.ini; $w/$h = expected frame size (h = $null: width only, height checked from
    # the stored eye heights); $tag = stereo_mode; $side = 'h' (left | right halves) or 'v' (top / bottom halves)
    function LayoutCase($name, $key, $cfgVal, $w, $h, $tag, $side, $cmdRe, $logRe) {
        Write-Host "`n=== ${name}: layout_3d = $cfgVal (640x360 eyes, short side 432)" -ForegroundColor Cyan
        WriteConfig 0 1 1 $cfgVal
        $src = $ld[$key]
        Step "${name}: job ended" (RunJob $src)
        $y = [IO.Path]::ChangeExtension($src, $null).TrimEnd('.') + '_YT8K.mkv'
        Step "${name}: only _YT8K.mkv made" ((Test-Path $y) -and -not (Test-Path ([IO.Path]::ChangeExtension($src, '.mkv'))))
        if (Test-Path $y) {
            $s = Probe $y 'stream=codec_name,width,height,pix_fmt:stream_tags=stereo_mode'
            $fw = [int]$s.width; $fh = [int]$s.height
            Write-Host ("  {0}x{1} stereo_mode={2}" -f $fw, $fh, $s.'TAG:stereo_mode')
            $sizeOk = ($fw -eq $w) -and ($(if ($h) { $fh -eq $h } else { $fh -ge 482 -and $fh -le 490 -and $fh % 2 -eq 0 }))
            Step "${name}: hevc yuv420p, frame ${fw}x${fh} (want ${w}x$(if ($h) { $h } else { '~486 even' }))" ($s.codec_name -eq 'hevc' -and $s.pix_fmt -eq 'yuv420p' -and $sizeOk -and $fw % 2 -eq 0 -and $fh % 2 -eq 0)
            Step "${name}: stereo_mode $tag (tag)" ($s.'TAG:stereo_mode' -eq $tag)
            if ($side -eq 'h') { $pw = $fw / 2; $oL = Avg $y ("{0}:{1}:0:0" -f $pw, $fh); $oR = Avg $y ("{0}:{1}:{0}:0" -f $pw, $fh) }
            else { $ph = $fh / 2; $oL = Avg $y ("{0}:{1}:0:0" -f $fw, $ph); $oR = Avg $y ("{0}:{1}:0:{1}" -f $fw, $ph) }
            Write-Host ("  source left {0} right {1} | {2} first {3} second {4}" -f (Fmt $srcL), (Fmt $srcR), $(if ($side -eq 'h') { 'left half' } else { 'top half' }), (Fmt $oL), (Fmt $oR))
            Step "${name}: first half = left eye, second half = right eye (within 10)" ((Near $oL $srcL 10) -and (Near $oR $srcR 10) -and (Dist $oL $oR) -gt 60)
        }
        $l = LogText
        Step "${name}: log has the layout line" ($l -match $logRe)
        Step "${name}: log has the 8K cmd with $tag graph" ($l -match $cmdRe)
    }
    $srcL = Avg $ld['sbs_half'] '640:360:0:0'; $srcR = Avg $ld['sbs_half'] '640:360:640:0'   # case A deleted its MP4: use the copy
    LayoutCase 'D' 'sbs_half' 'sbs_half' 768 432 'left_right' 'h' `
        'job: 8K cmd \(libx265\): .*scale=w=2\*round\(iw\*min\(432/min\(iw\\,ih\)\\,8192/max\(iw\\,ih\)\)/4\):h=2\*round\(ih\*min\(.*\)/2\):flags=lanczos\[L\].*\[L\]\[R\]hstack=inputs=2\,setsar=1.* stereo_mode=left_right .*ylsbshalf_SBS_YT8K\.mkv' `
        'layout_3d sbs_half: frame = one eye''s aspect, each eye squeezed to half width, left eye left; frame short side 432'
    LayoutCase 'E' 'sbs_full' 'sbs_full' 1536 432 'left_right' 'h' `
        'job: 8K cmd \(libx265\): .*scale=w=2\*round\(iw\*min\(432/min\(2\*iw\\,ih\)\\,8192/max\(2\*iw\\,ih\)\)/2\):h=2\*round\(ih\*min\(.*\)/2\):flags=lanczos\[L\].*\[L\]\[R\]hstack=inputs=2\,setsar=1.* stereo_mode=left_right .*ylsbsfull_SBS_YT8K\.mkv' `
        'layout_3d sbs_full: frame = 2x one eye''s aspect \(side by side\), each eye at its own aspect; frame short side 432'
    LayoutCase 'F' 'tb_full' 'tb_full' 432 $null 'top_bottom' 'v' `
        'job: 8K cmd \(libx265\): .*scale=w=2\*round\(iw\*min\(432/min\(iw\\,2\*ih\)\\,8192/max\(iw\\,2\*ih\)\)/2\):h=2\*round\(ih\*min\(.*\)/2\):flags=lanczos\[L\].*\[L\]\[R\]vstack=inputs=2\,setsar=1.* stereo_mode=top_bottom .*yltbfull_SBS_YT8K\.mkv' `
        'layout_3d tb_full: frame = one eye''s aspect / 2 \(top-bottom\), each eye at its own aspect; frame short side 432'
    # G: unknown value -> tb_half + a warning
    LayoutCase 'G' 'bogus' 'bogus' 768 432 'top_bottom' 'v' `
        'job: 8K cmd \(libx265\): .*vstack=inputs=2\,setsar=1.* stereo_mode=top_bottom .*ylbogus_SBS_YT8K\.mkv' `
        'layout_3d tb_half: frame = one eye''s aspect'
    Step "G: warning for the bad value in the log" ((LogText) -match "bad value 'bogus' for 'layout_3d'")

    # ---- H: MKV step with layout_3d = tb_half (the default): re-encode at the native size
    Write-Host "`n=== H: post_mkv 1, layout_3d = tb_half (640x360 eyes), keep_mp4 0 -> re-encoded .mkv" -ForegroundColor Cyan
    WriteConfig 1 0 0 'tb_half'
    Step 'H: job ended' (RunJob $mh)
    $y = Join-Path $c 'yh_SBS.mkv'
    Step 'H: .mkv made, no _YT8K.mkv, MP4 deleted after the verification' ((Test-Path $y) -and -not (Test-Path (Join-Path $c 'yh_SBS_YT8K.mkv')) -and -not (Test-Path $mh))
    if (Test-Path $y) {
        $s = Probe $y 'stream=codec_name,width,height,pix_fmt,r_frame_rate:stream_tags=stereo_mode:format=duration'
        Write-Host ("  {0}x{1} {2} {3} stereo_mode={4} duration={5}" -f $s.width, $s.height, $s.codec_name, $s.r_frame_rate, $s.'TAG:stereo_mode', $s.duration)
        Step 'H: hevc 640x360 yuv420p 30/1 (one eye''s size: re-encoded, not the 1280x360 remux)' ($s.codec_name -eq 'hevc' -and $s.width -eq '640' -and $s.height -eq '360' -and $s.pix_fmt -eq 'yuv420p' -and $s.r_frame_rate -eq '30/1')
        Step 'H: stereo_mode top_bottom (tag)' ($s.'TAG:stereo_mode' -eq 'top_bottom')
        Step ('H: duration ~2 s ({0})' -f $s.duration) ([double]$s.duration -gt 1.9 -and [double]$s.duration -lt 2.15)
        $aud = & $ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 $y
        Step "H: audio copied ($aud)" ($aud -eq 'aac')
        $oL = Avg $y '640:180:0:0'; $oR = Avg $y '640:180:0:180'
        Write-Host ("  source left {0} right {1} | mkv top {2} bottom {3}" -f (Fmt $srcL), (Fmt $srcR), (Fmt $oL), (Fmt $oR))
        Step 'H: top half = left eye, bottom half = the blue eye (within 10)' ((Near $oL $srcL 10) -and (Near $oR $srcR 10) -and (Dist $oL $oR) -gt 60)
    }
    $l = LogText
    Step 'H: log: layout line (native size, re-encode)' ($l -match 'job: MKV step: layout_3d tb_half: frame = one eye''s aspect, each eye squeezed to half height, left eye on top; native size')
    Step 'H: log: MKV libx265 cmd with the native graph (min(1, 8192/..)), vstack, top_bottom, no fps cap' ($l -match 'job: MKV cmd \(libx265\): .*-vf "split=2\[l\]\[r\];\[l\]crop=iw/2:ih:0:0\,scale=w=2\*round\(iw\*min\(1\\,8192/max\(iw\\,ih\)\)/2\):h=2\*round\(ih\*min\(1\\,.*\)/4\):flags=lanczos\[L\].*\[L\]\[R\]vstack=inputs=2\,setsar=1\,format=yuv420p.* -c:a copy .* stereo_mode=top_bottom .*yh_SBS\.mkv')
    Step 'H: log: verification line (exists, size > 0, duration vs MP4)' ($l -match 'job: MKV verification \(re-encode\): exists, \d+ bytes > 0, duration [\d.]+ s vs MP4 [\d.]+ s \(within [\d.]+ s\) -- ok')
    Step 'H: log: MKV step ok (re-encoded), MP4 deleted afterwards' ($l -match 'job: MKV step ok in [\d.]+ s \(re-encoded, \d+ bytes, MP4 \d+ bytes\) -> yh_SBS\.mkv' -and $l -match 'job: deleted yh_SBS\.mp4 \(keep_mp4 = 0\)')

    # ---- I: MKV step with layout_3d = sbs_full: plain remux
    Write-Host "`n=== I: post_mkv 1, layout_3d = sbs_full, keep_mp4 0 -> plain remux" -ForegroundColor Cyan
    WriteConfig 1 0 0 'sbs_full'
    Step 'I: job ended' (RunJob $mi)
    $y = Join-Path $c 'yi_SBS.mkv'
    Step 'I: .mkv made, MP4 deleted' ((Test-Path $y) -and -not (Test-Path $mi))
    if (Test-Path $y) {
        $s = Probe $y 'stream=codec_name,width,height,r_frame_rate:stream_tags=stereo_mode'
        Write-Host ("  {0}x{1} {2} {3} stereo_mode={4}" -f $s.width, $s.height, $s.codec_name, $s.r_frame_rate, $s.'TAG:stereo_mode')
        Step 'I: stream copy: h264 1280x360 (the recording as it is), 30/1' ($s.codec_name -eq 'h264' -and $s.width -eq '1280' -and $s.height -eq '360' -and $s.r_frame_rate -eq '30/1')
        Step 'I: stereo_mode left_right (tag)' ($s.'TAG:stereo_mode' -eq 'left_right')
    }
    $l = LogText
    Step 'I: log: plain remux (-c copy), size verification line' ($l -match 'job: MKV step: layout_3d sbs_full \+ eye_crop none -- plain remux \(-c copy\)' -and $l -match 'job: MKV cmd: .*-map 0 -c copy -metadata:s:v:0 stereo_mode=left_right .*yi_SBS\.mkv' -and $l -match 'job: MKV verification \(remux\): exists, \d+ bytes = [\d.]+% of the MP4''s \d+ bytes \(> 90%\) -- ok')

    # ---- J..L: eye_crop on TALL eyes (640x900), 8K only (S = 432), tb_half, MP4 kept
    function CropCase($name, $key, $crop, $w, $h, $cropLog) {
        Write-Host "`n=== ${name}: eye_crop = $crop (640x900 eyes, tb_half, short side 432)" -ForegroundColor Cyan
        WriteConfig 0 1 1 'tb_half' $crop
        $src = $tc[$key]
        Step "${name}: job ended" (RunJob $src)
        $y = [IO.Path]::ChangeExtension($src, $null).TrimEnd('.') + '_YT8K.mkv'
        Step "${name}: _YT8K.mkv made" (Test-Path $y)
        if (Test-Path $y) {
            $s = Probe $y 'stream=codec_name,width,height:stream_tags=stereo_mode'
            Write-Host ("  {0}x{1} stereo_mode={2}" -f $s.width, $s.height, $s.'TAG:stereo_mode')
            Step "${name}: frame ${w}x${h}, top_bottom" ([int]$s.width -eq $w -and [int]$s.height -eq $h -and $s.'TAG:stereo_mode' -eq 'top_bottom')
            $hh = [int]($h / 2)
            $oR = Avg $y ("{0}:{1}:0:{1}" -f $w, $hh)
            Step "${name}: bottom half = right eye (solid orange, no stretch artefacts)" (Near $oR @(200, 60, 30) 12)
        }
        $l = LogText
        Step "${name}: log shows the crop and the frame size" ($l -match [regex]::Escape("8K step: input 1280x900 = eyes 640x900; eye_crop $crop -> $cropLog") -and $l -match ("tb_half frame {0}x{1}" -f $w, $h))
    }
    CropCase 'J' 'c169' '16:9' 768 432 'centre-cropped to 640x360'
    CropCase 'K' 'c43' '4:3' 576 432 'centre-cropped to 640x480'
    CropCase 'L' 'cnone' 'none' 432 608 'no crop needed, eyes 640x900'
    $l = LogText
    Step 'J: crop expression in the logged command' ($l -match 'crop=iw/2:min\(ih\\,2\*trunc\(iw/2\*9/16/2\)\):0:trunc\(\(ih-oh\)/4\)\*2')

    # ---- M: eye_crop in the MKV step (native size, re-encode): 640x900 eyes -> 16:9 -> 640x360 eyes, tb_half frame 640x360
    Write-Host "`n=== M: post_mkv 1, eye_crop 16:9, tb_half (640x900 eyes) -> .mkv 640x360" -ForegroundColor Cyan
    WriteConfig 1 0 0 'tb_half' '16:9'
    Step 'M: job ended' (RunJob $tc['cmkv'])
    $mm = Join-Path $c 'ytcmkv_SBS.mkv'
    Step 'M: .mkv made, MP4 deleted' ((Test-Path $mm) -and -not (Test-Path $tc['cmkv']))
    if (Test-Path $mm) {
        $s = Probe $mm 'stream=codec_name,width,height:stream_tags=stereo_mode'
        Write-Host ("  {0}x{1} stereo_mode={2}" -f $s.width, $s.height, $s.'TAG:stereo_mode')
        Step 'M: hevc 640x360 (16:9 frame), top_bottom' ($s.codec_name -eq 'hevc' -and [int]$s.width -eq 640 -and [int]$s.height -eq 360 -and $s.'TAG:stereo_mode' -eq 'top_bottom')
    }
    Step 'M: log shows crop + frame' ((LogText) -match 'MKV step: input 1280x900 = eyes 640x900; eye_crop 16:9 -> centre-cropped to 640x360; tb_half frame 640x360')

    # ---- C: dome recording -> skipped
    Write-Host "`n=== C: _VR180.mp4 + vr180 sidecar, post_mkv 0 + post_yt8k 1" -ForegroundColor Cyan
    WriteConfig 0 1 1
    Step 'C: job ended' (RunJob $v)
    $l = LogText
    Step 'C: skipped, logged, no _YT8K.mkv, MP4 kept' ($l -match 'job: 8K step skipped: VR180 recordings are already full-frame' -and -not (Test-Path (Join-Path $c 'yc_VR180_YT8K.mkv')) -and (Test-Path $v))
    Step 'C: balloon "Nothing to convert"' ($l -match 'balloon \[Nothing to convert\] yc_VR180\.mp4\s+VR180 recordings are already full-frame')

    Write-Host "`n--- tray.log (job lines):"
    (LogText) -split "`r?`n" | Where-Object { $_ -match 'job: |balloon' } | ForEach-Object { Write-Host "  $_" }
    Step 'no job FAILED' (-not ((LogText) -match 'job: FAILED'))
} finally {
    $env:APPDATA = $oldAppdata
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_NO_NVENC', 'EYECAP_TRAY_TEST_YT8K_SHORT') { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
}
if ($fail) { Write-Host "`n$fail check(s) FAILED" -ForegroundColor Red; exit 1 } else { Write-Host "`nall checks passed" -ForegroundColor Green; exit 0 }
