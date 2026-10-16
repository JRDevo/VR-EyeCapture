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
    Y. a recording made with record_for_youtube = 1 (packed: 768x432 = two 768x216 eyes stacked, top testsrc2, bottom solid
       blue, 30 fps, 2 s, AAC) + its sidecar {"projection":"flat","layout":"top_bottom","packing":"tb_half","eye_crop":"16:9",
       ...,"youtube":1}; post_mkv 1, post_yt8k 1, post_flat 1, keep_mp4 0, layout_3d sbs_half / eye_crop 4:3 (must be
       ignored): <name>.mkv is a STREAM COPY (h264 768x432 like the MP4, stereo_mode=top_bottom, no crop tag), logged
       "recorded in the YouTube layout (tb_half, 16:9, 768x432): remux only" and "already in the YouTube 8K layout -- 8K
       step skipped" (no 8K command, no _YT8K file); MP4 + sidecar deleted after the remux verification; _FLAT.mp4 is
       3840x2160 with square pixels and the top eye's colours (crop=iw:ih/2:0:0,scale=iw:ih*2,setsar=1 first); balloon
       "Ready for YouTube: <name>.mkv".
    N. never overwrite + crop tag: one MP4 (keep_mp4 1, 8K only) converted with eye_crop 16:9, then 4:3, then 4:3 again:
       _YT8K.mkv, _YT8K_4x3.mkv and _YT8K_4x3 (2).mkv all exist, earlier files untouched, log "exists -- writing ... instead".
    J/K/K2/L. eye_crop 16:9 / 4:3 / 4:3pad / none on tall 640x900 eyes, 8K only, tb_half: frame size, name tag, bottom half =
       the right eye; K2 (4:3pad): 640x480 padded to 854x480 -> 768x432 (picture 576 wide at x 96), <name>_YT8K_4x3pad.mkv,
       both bars black, the pad filter in the logged command.
    C. a _VR180.mp4 with its vr180 sidecar; post_mkv 0, post_yt8k 1: "8K step skipped: VR180 ..." logged, no _YT8K.mkv,
       balloon "Nothing to convert".
    Q. yt8k_codec = av1 (8K only, yt8k_bitrate_mbps 100) on this NO_NVENC tray: the logged plan is av1_nvenc with the same
       VBR maths (-b:v 100M -maxrate 133M -bufsize 200M, -tier 1 -highbitdepth 0 -multipass qres), then "av1_nvenc not
       available -- using hevc_nvenc (yt8k_codec av1: falling back to HEVC)", "hevc_nvenc not available -- using libx265";
       the 8K file is hevc 768x432 with the stereo_mode=top_bottom tag.
    S. an AV1 source (as a codec = av1 recording would be; made with libaom-av1, skipped when this ffmpeg has none):
       post_mkv 1 with sbs_full + eye_crop none = plain remux -> the .mkv is av1 1280x360 left_right; post_yt8k 1 -> hevc
       1536x432 left_right (sbs_full); log "source video is AV1"; the libx265 command has no -hwaccel (software encodes decode in software).
    R. NVENC "available" without a GPU: a second test tray WITHOUT EYECAP_TRAY_TEST_NO_NVENC runs build\Release\fake_ffmpeg.exe
       (FAKE_FFMPEG_ENCODERS=hevc_nvenc,av1_nvenc; it copies input to output). R1 yt8k_codec av1: the file is
       <name>_YT8K_full_AV1.mkv (av1_nvenc made it); R2 and Q: the fallback encoder made it, so it is renamed to the plain
       <name>_YT8K_full.mkv (logged "made by X, not av1_nvenc -- renamed"). R1: the 8K command is
       "8K cmd (av1_nvenc): ... -c:v av1_nvenc -preset p5 -tune hq -multipass qres -tier 1 -highbitdepth 0 -rc vbr -b:v 150M
       -maxrate 200M -bufsize 300M ..." and no hevc_nvenc command follows. R2 with FAKE_FFMPEG_FAIL_ON=av1_nvenc (listed but
       cannot open, like a GPU without AV1 NVENC): "av1_nvenc failed (...) -- retrying with hevc_nvenc (yt8k_codec av1:
       falling back to HEVC)", then the hevc_nvenc command, job done.
    P. progress / pause / cancel on a LONGER source (2560x720 SBS, 120 s, so the libx265 MKV re-encode runs ~15+ s). The
       test posts WM_APP+5 (pause / resume toggle) and WM_APP+6 (cancel) to the TEST tray's message window (found by
       class "OpenXREyeCaptureTray" AND the test tray's pid -- never the user's real tray).
       P1 (post_mkv 1, keep_mp4 1): progress logged ("MKV progress 25%", tooltip line "converting ... (MKV) N%"); pause ->
          "SUSPENDED (paused by user)", only <name>.mkv.part exists while the step runs and does not grow for 2 s, tooltip
          line "paused: ..."; resume -> the job finishes ("job: done", the .part renamed to the .mkv).
       P2 (post_mkv 1, keep_mp4 0) + a short job queued behind it: pause, then cancel while paused -> ffmpeg resumed then
          ended (its pid gone), partial .mkv.part deleted, MP4 KEPT although keep_mp4 = 0, "job: cancelled by user" logged,
          balloon "Conversion cancelled: ...", no "job: FAILED"; the queued job still converts. Cancel with no job running
          is a logged no-op.
    The test tray runs in test mode: balloons are only logged, never shown on the desktop.
#>
param([string]$FfmpegDir = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root 'build\Release'
foreach ($f in @('EyeCaptureTray.exe', 'status_sim.exe', 'fake_ffmpeg.exe')) { if (-not (Test-Path (Join-Path $bin $f))) { throw "build first: $f" } }
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
function WriteConfig($mkv, $yt8k, $keep, $layout = 'tb_half', $crop = 'none', $br8k = '150', $codec8k = 'hevc', $ffPath = $ffmpeg, $flat = '0') {
    # the tray re-reads config.ini for every job, so the layout can change between cases
    $s = $baseIni -replace '(?m)^post_mkv = \S+', "post_mkv = $mkv" -replace '(?m)^post_flat = \S+', "post_flat = $flat" `
                  -replace '(?m)^post_yt8k = \S+', "post_yt8k = $yt8k" -replace '(?m)^keep_mp4 = \S+', "keep_mp4 = $keep" `
                  -replace '(?m)^layout_3d = \S+', "layout_3d = $layout" -replace '(?m)^eye_crop = \S+', "eye_crop = $crop" `
                  -replace '(?m)^post_when = \S+', 'post_when = now' -replace '(?m)^yt8k_bitrate_mbps = \S+', "yt8k_bitrate_mbps = $br8k" `
                  -replace '(?m)^ffmpeg_path =.*$', "ffmpeg_path = $ffPath" -replace '(?m)^log_level = \S+', 'log_level = info'
    # yt8k_codec: replaced when the base config has the key, else appended (the tray reads keys in any section)
    if ($s -match '(?m)^yt8k_codec = ') { $s = $s -replace '(?m)^yt8k_codec = \S+', "yt8k_codec = $codec8k" } else { $s += "`r`nyt8k_codec = $codec8k`r`n" }
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
# case N (never overwrite) needs its own copy too; Q (yt8k_codec av1) and R1 / R2 (fake ffmpeg) too
$qsrc = Join-Path $c 'yq8_SBS.mp4'; Copy-Item $a $qsrc
$r1 = Join-Path $c 'yr1_SBS.mp4'; $r2 = Join-Path $c 'yr2_SBS.mp4'; Copy-Item $a $r1; Copy-Item $a $r2; $sIn = Join-Path $c 'ysin_SBS.mp4'; Copy-Item $a $sIn   # S encodes its AV1 source from this copy
$nsrc0 = Join-Path $c 'yn0_SBS.mp4'; Copy-Item $a $nsrc0
# eye_crop cases J..M: TALL eyes (640x900 each, 1280x900 SBS), solid orange right eye, copies per case
$tall = Join-Path $c 'yt_SBS.mp4'
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=640x900:rate=30:duration=2' -f lavfi -i 'color=c=0xC83C1E:size=640x900:rate=30:duration=2' `
    -filter_complex '[0:v][1:v]hstack=inputs=2,format=yuv420p[v]' -map '[v]' -c:v libx264 -preset veryfast -crf 16 $tall
$tc = @{}
foreach ($k in 'c169', 'c43', 'c43pad', 'cnone', 'cmkv') { $tc[$k] = Join-Path $c ("yt{0}_SBS.mp4" -f $k); Copy-Item $tall $tc[$k] }
Step 'tall-eye sources made' ((Test-Path $tall) -and (Test-Path $tc['cmkv']))
# MKV-step cases H / I (layout_3d tb_half re-encode / sbs_full remux)
$mh = Join-Path $c 'yh_SBS.mp4'; $mi = Join-Path $c 'yi_SBS.mp4'; Copy-Item $a $mh; Copy-Item $a $mi
# case Y: a recording made with record_for_youtube = 1 (what the layer writes): already tb_half, 16:9 eyes (768x216 each, stacked
# = 768x432; top = left eye testsrc2, bottom = right eye solid blue), 30 fps, 2 s, AAC, plus its one-line sidecar
$py = Join-Path $c 'yy_TB.mp4'; $pyJson = [IO.Path]::ChangeExtension($py, '.json')
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=768x216:rate=30:duration=2' -f lavfi -i 'color=c=0x1E3CC8:size=768x216:rate=30:duration=2' `
    -f lavfi -i 'sine=frequency=440:duration=2' -filter_complex '[0:v][1:v]vstack=inputs=2,format=yuv420p[v]' -map '[v]' -map 2:a `
    -c:v libx264 -preset veryfast -crf 16 -c:a aac -shortest $py
'{"projection":"flat","layout":"top_bottom","packing":"tb_half","eye_crop":"16:9","eyes":2,"width":768,"height":432,"youtube":1}' |
    Set-Content $pyJson -NoNewline -Encoding ascii
Step 'packed (YouTube-layout) source + sidecar made' ((Test-Path $py) -and (Test-Path $pyJson))
# case P (progress / pause / cancel): a LONG source so the job runs for a while; yq = a short job queued behind P2
$p1 = Join-Path $c 'yp1_SBS.mp4'; $p2 = Join-Path $c 'yp2_SBS.mp4'; $pq = Join-Path $c 'yq_SBS.mp4'
& $ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=1280x720:rate=30:duration=120' -f lavfi -i 'color=c=0x1E3CC8:size=1280x720:rate=30:duration=120' `
    -f lavfi -i 'sine=frequency=440:duration=120' -filter_complex '[0:v][1:v]hstack=inputs=2,format=yuv420p[v]' -map '[v]' -map 2:a `
    -c:v libx264 -preset veryfast -crf 16 -c:a aac -shortest $p1
Copy-Item $p1 $p2; Copy-Item $a $pq
Step 'long source for P made' ((Test-Path $p1) -and (Test-Path $p2) -and (Test-Path $pq))

Add-Type -Namespace Yt8kTest -Name W -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
public static IntPtr TrayWindow(uint pid) {
    IntPtr w = IntPtr.Zero;
    while ((w = FindWindowExW(new IntPtr(-3), w, "OpenXREyeCaptureTray", null)) != IntPtr.Zero) {
        uint p; GetWindowThreadProcessId(w, out p);
        if (p == pid) return w;
    }
    return IntPtr.Zero;
}
'@

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
# start the test tray (its own shm name, no NVENC, small 8K short side); returns the process
# -Nvenc (case R only, with fake_ffmpeg): EYECAP_TRAY_TEST_NO_NVENC is NOT set, so the tray uses the NVENC encoders ffmpeg lists
function StartTray([switch]$Nvenc) {
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-yt8k-test'; $env:EYECAP_TRAY_TEST_YT8K_SHORT = '432'
    if (-not $Nvenc) { $env:EYECAP_TRAY_TEST_NO_NVENC = '1' }
    $env:APPDATA = Join-Path $t 'appdata'
    $proc = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_NO_NVENC', 'EYECAP_TRAY_TEST_YT8K_SHORT') { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
    $env:APPDATA = $oldAppdata
    Start-Sleep -Seconds 2
    return $proc
}
$p = $null
try {
    WriteConfig 1 1 0
    $p = StartTray

    # ---- A: wide eyes, 30 fps, MKV + 8K, MP4 deleted
    Write-Host "`n=== A: 1280x360 SBS, 30 fps, post_mkv 1 + post_yt8k 1, keep_mp4 0" -ForegroundColor Cyan
    $srcL = Avg $a '640:360:0:0'; $srcR = Avg $a '640:360:640:0'
    Step 'A: job ended' (RunJob $a)
    $y = Join-Path $c 'ya_SBS_YT8K_full.mkv'
    Step 'A: _YT8K.mkv and .mkv exist, MP4 deleted (keep_mp4 0)' ((Test-Path $y) -and (Test-Path (Join-Path $c 'ya_SBS_full.mkv')) -and -not (Test-Path $a))
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
    Step 'A: log has the 8K libx265 command with the vstack expression graph' ($l -match 'job: 8K cmd \(libx265\): .*split=2\[l\]\[r\];\[l\]crop=iw/2:ih:0:0\,scale=w=2\*round\(iw\*min\(432/min\(iw\\,ih\)\\,8192/max\(iw\\,ih\)\)/2\):h=2\*round\(ih\*min\(.*\)/4\):flags=lanczos\[L\];\[r\]crop=iw/2:ih:iw/2:0\,.*\[L\]\[R\]vstack=inputs=2\,setsar=1\,format=yuv420p.* -metadata:s:v:0 stereo_mode=top_bottom .*ya_SBS_YT8K_full\.mkv')
    Step 'A: log: layout + test short side + 30 fps kept + NO_NVENC' ($l -match 'layout_3d tb_half: frame = one eye''s aspect, each eye squeezed to half height, left eye on top; frame short side 432' -and $l -match 'EYECAP_TRAY_TEST_YT8K_SHORT=432 \(test\)' -and $l -match 'source r_frame_rate 30/1 .* <= 60 -- frame rate kept' -and $l -match 'EYECAP_TRAY_TEST_NO_NVENC is set')
    Step 'A: log: yt8k_bitrate_mbps 150 (default) -> NVENC -b:v 150M -maxrate 200M -bufsize 300M, x265 crf 18' ($l -match 'job: 8K step: yt8k_bitrate_mbps 150 -> hevc_nvenc -c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v 150M -maxrate 200M -bufsize 300M \(libx265 fallback: crf 18, no bitrate\)')
    Step 'A: balloon names the MKV and the 8K file' ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube: ya_SBS_full\.mkv\s+YouTube 8K: ya_SBS_YT8K_full\.mkv')

    # ---- B: tall eyes, 72 fps -> 60, 8K only
    Write-Host "`n=== B: 960x720 SBS (tall eyes), 72 fps, post_mkv 0 + post_yt8k 1, keep_mp4 0" -ForegroundColor Cyan
    WriteConfig 0 1 0 'tb_half' 'none' '100'   # also: yt8k_bitrate_mbps 100 -> 100M / 133M / 200M
    $srcL = Avg $b '480:720:0:0'; $srcR = Avg $b '480:720:480:0'
    Step 'B: job ended' (RunJob $b)
    $y = Join-Path $c 'yb_SBS_YT8K_full.mkv'
    Step 'B: only _YT8K.mkv made, no .mkv, MP4 kept (no MKV step)' ((Test-Path $y) -and -not (Test-Path (Join-Path $c 'yb_SBS_full.mkv')) -and (Test-Path $b))
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
    Step 'B: log: 72 fps source capped to 60' ($l -match 'source r_frame_rate 72/1 \(72\.000 fps, avg 72/1\) > 60 -- capped to 60 fps' -and $l -match 'job: 8K cmd \(libx265\): .*-vf "fps=60,split=2.*yb_SBS_YT8K_full\.mkv')
    Step 'B: log: yt8k_bitrate_mbps 100 -> -b:v 100M -maxrate 133M -bufsize 200M' ($l -match 'job: 8K step: yt8k_bitrate_mbps 100 -> hevc_nvenc .*-b:v 100M -maxrate 133M -bufsize 200M')
    Step 'B: balloon "Ready for YouTube (8K)"' ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube \(8K\): yb_SBS_YT8K_full\.mkv')

    # ---- D..G: the other layouts on the case-A source (640x360 eyes, S = 432), 8K only, MP4 kept
    # $key = file key, $cfgVal = value written to config.ini; $w/$h = expected frame size (h = $null: width only, height checked from
    # the stored eye heights); $tag = stereo_mode; $side = 'h' (left | right halves) or 'v' (top / bottom halves)
    function LayoutCase($name, $key, $cfgVal, $w, $h, $tag, $side, $cmdRe, $logRe) {
        Write-Host "`n=== ${name}: layout_3d = $cfgVal (640x360 eyes, short side 432)" -ForegroundColor Cyan
        WriteConfig 0 1 1 $cfgVal
        $src = $ld[$key]
        Step "${name}: job ended" (RunJob $src)
        $y = [IO.Path]::ChangeExtension($src, $null).TrimEnd('.') + '_YT8K_full.mkv'
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
        'job: 8K cmd \(libx265\): .*scale=w=2\*round\(iw\*min\(432/min\(iw\\,ih\)\\,8192/max\(iw\\,ih\)\)/4\):h=2\*round\(ih\*min\(.*\)/2\):flags=lanczos\[L\].*\[L\]\[R\]hstack=inputs=2\,setsar=1.* stereo_mode=left_right .*ylsbshalf_SBS_YT8K_full\.mkv' `
        'layout_3d sbs_half: frame = one eye''s aspect, each eye squeezed to half width, left eye left; frame short side 432'
    LayoutCase 'E' 'sbs_full' 'sbs_full' 1536 432 'left_right' 'h' `
        'job: 8K cmd \(libx265\): .*scale=w=2\*round\(iw\*min\(432/min\(2\*iw\\,ih\)\\,8192/max\(2\*iw\\,ih\)\)/2\):h=2\*round\(ih\*min\(.*\)/2\):flags=lanczos\[L\].*\[L\]\[R\]hstack=inputs=2\,setsar=1.* stereo_mode=left_right .*ylsbsfull_SBS_YT8K_full\.mkv' `
        'layout_3d sbs_full: frame = 2x one eye''s aspect \(side by side\), each eye at its own aspect; frame short side 432'
    LayoutCase 'F' 'tb_full' 'tb_full' 432 $null 'top_bottom' 'v' `
        'job: 8K cmd \(libx265\): .*scale=w=2\*round\(iw\*min\(432/min\(iw\\,2\*ih\)\\,8192/max\(iw\\,2\*ih\)\)/2\):h=2\*round\(ih\*min\(.*\)/2\):flags=lanczos\[L\].*\[L\]\[R\]vstack=inputs=2\,setsar=1.* stereo_mode=top_bottom .*yltbfull_SBS_YT8K_full\.mkv' `
        'layout_3d tb_full: frame = one eye''s aspect / 2 \(top-bottom\), each eye at its own aspect; frame short side 432'
    # G: unknown value -> tb_half + a warning
    LayoutCase 'G' 'bogus' 'bogus' 768 432 'top_bottom' 'v' `
        'job: 8K cmd \(libx265\): .*vstack=inputs=2\,setsar=1.* stereo_mode=top_bottom .*ylbogus_SBS_YT8K_full\.mkv' `
        'layout_3d tb_half: frame = one eye''s aspect'
    Step "G: warning for the bad value in the log" ((LogText) -match "bad value 'bogus' for 'layout_3d'")

    # ---- H: MKV step with layout_3d = tb_half (the default): re-encode at the native size
    Write-Host "`n=== H: post_mkv 1, layout_3d = tb_half (640x360 eyes), keep_mp4 0 -> re-encoded .mkv" -ForegroundColor Cyan
    WriteConfig 1 0 0 'tb_half'
    Step 'H: job ended' (RunJob $mh)
    $y = Join-Path $c 'yh_SBS_full.mkv'
    Step 'H: .mkv made, no _YT8K.mkv, MP4 deleted after the verification' ((Test-Path $y) -and -not (Test-Path (Join-Path $c 'yh_SBS_YT8K_full.mkv')) -and -not (Test-Path $mh))
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
    Step 'H: log: MKV libx265 cmd with the native graph (min(1, 8192/..)), vstack, top_bottom, no fps cap' ($l -match 'job: MKV cmd \(libx265\): .*-vf "split=2\[l\]\[r\];\[l\]crop=iw/2:ih:0:0\,scale=w=2\*round\(iw\*min\(1\\,8192/max\(iw\\,ih\)\)/2\):h=2\*round\(ih\*min\(1\\,.*\)/4\):flags=lanczos\[L\].*\[L\]\[R\]vstack=inputs=2\,setsar=1\,format=yuv420p.* -c:a copy .* stereo_mode=top_bottom .*yh_SBS_full\.mkv')
    Step 'H: log: verification line (exists, size > 0, duration vs MP4)' ($l -match 'job: MKV verification \(re-encode\): exists, \d+ bytes > 0, duration [\d.]+ s vs MP4 [\d.]+ s \(within [\d.]+ s\) -- ok')
    Step 'H: log: MKV step ok (re-encoded), MP4 deleted afterwards' ($l -match 'job: MKV step ok in [\d.]+ s \(re-encoded, \d+ bytes, MP4 \d+ bytes\) -> yh_SBS_full\.mkv' -and $l -match 'job: deleted yh_SBS\.mp4 \(keep_mp4 = 0\)')

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

    # ---- Y: recorded in the YouTube layout (record_for_youtube = 1, sidecar "youtube":1): the MKV step is a plain remux
    # whatever layout_3d / eye_crop say (sbs_half / 4:3 here, to prove they are ignored), the 8K step is skipped (counts as ok:
    # keep_mp4 0 deletes the MP4 after the verified remux), the flat step takes the top eye and un-squeezes it
    Write-Host "`n=== Y: packed tb_half 768x432 + sidecar, post_mkv 1 + post_yt8k 1 + post_flat 1, keep_mp4 0, layout_3d sbs_half / eye_crop 4:3 (ignored)" -ForegroundColor Cyan
    WriteConfig 1 1 0 'sbs_half' '4:3' '150' 'hevc' $ffmpeg '1'
    $ySrc = Probe $py 'stream=codec_name,width,height'
    $ySize = (Get-Item $py).Length
    $yTop = Avg $py '768:216:0:0'; $yBot = Avg $py '768:216:0:216'
    Step 'Y: job ended' (RunJob $py)
    $ym = Join-Path $c 'yy_TB.mkv'; $yf = Join-Path $c 'yy_TB_FLAT.mp4'
    $y8 = @(Get-ChildItem $c -Filter 'yy_TB_YT8K*' -ErrorAction SilentlyContinue)
    Step 'Y: <name>.mkv (no crop tag) and _FLAT.mp4 made, no _YT8K file, MP4 and sidecar deleted (keep_mp4 0)' ((Test-Path $ym) -and (Test-Path $yf) -and $y8.Count -eq 0 -and -not (Test-Path (Join-Path $c 'yy_TB_4x3.mkv')) -and -not (Test-Path $py) -and -not (Test-Path $pyJson))
    if (Test-Path $ym) {
        $s = Probe $ym 'stream=codec_name,width,height,r_frame_rate:stream_tags=stereo_mode'
        $mSize = (Get-Item $ym).Length
        Write-Host ("  mkv: {0}x{1} {2} {3} stereo_mode={4}, {5} bytes (MP4 {6} bytes)" -f $s.width, $s.height, $s.codec_name, $s.r_frame_rate, $s.'TAG:stereo_mode', $mSize, $ySize)
        Step 'Y: stream copy: same codec and size as the MP4 (h264 768x432), stereo_mode top_bottom' ($s.codec_name -eq $ySrc.codec_name -and $s.codec_name -eq 'h264' -and $s.width -eq $ySrc.width -and $s.height -eq $ySrc.height -and $s.width -eq '768' -and $s.height -eq '432' -and $s.'TAG:stereo_mode' -eq 'top_bottom')
        $aud = & $ffprobe -v error -select_streams a -show_entries stream=codec_name -of csv=p=0 $ym
        Step "Y: audio copied ($aud)" ($aud -eq 'aac')
        $mTop = Avg $ym '768:216:0:0'
        Step 'Y: the picture is unchanged (top half = the top eye)' (Near $mTop $yTop 6)
    }
    if (Test-Path $yf) {
        $s = Probe $yf 'stream=width,height,sample_aspect_ratio,display_aspect_ratio'
        Write-Host ("  flat: {0}x{1} SAR {2} DAR {3}" -f $s.width, $s.height, $s.sample_aspect_ratio, $s.display_aspect_ratio)
        Step 'Y: _FLAT.mp4 is 3840x2160, square pixels (16:9 shown)' ($s.width -eq '3840' -and $s.height -eq '2160' -and ($s.sample_aspect_ratio -eq '1:1' -or $s.sample_aspect_ratio -eq 'N/A' -or $s.sample_aspect_ratio -eq '0:1'))
        $fAll = Avg $yf '3840:2160:0:0'
        Write-Host ("  source top eye {0} bottom eye {1} | flat {2}" -f (Fmt $yTop), (Fmt $yBot), (Fmt $fAll))
        Step 'Y: _FLAT.mp4 shows the top (left) eye''s colours, not the blue eye' ((Near $fAll $yTop 10) -and (Dist $fAll $yBot) -gt 60)
    }
    $l = LogText
    Step 'Y: log: sidecar read as YouTube tb_half' ($l -match 'job: yy_TB\.json -> YouTube tb_half \(flat, recorded in the YouTube layout \(record_for_youtube\), Matroska stereo_mode=top_bottom\)')
    Step 'Y: log: "recorded in the YouTube layout (tb_half, 16:9, 768x432): remux only"' ($l -match 'job: MKV step: recorded in the YouTube layout \(tb_half, 16:9, 768x432\): remux only \(-c copy\), stereo_mode=top_bottom; this job''s layout_3d sbs_half / eye_crop 4:3 do not apply')
    Step 'Y: log: the remux command (stereo_mode=top_bottom) and the size verification' ($l -match 'job: MKV cmd: .*yy_TB\.mp4" -map 0 -c copy -metadata:s:v:0 stereo_mode=top_bottom -f matroska .*yy_TB\.mkv\.part' -and $l -match 'job: MKV step ok in [\d.]+ s \(remuxed, \d+ bytes, MP4 \d+ bytes\) -> yy_TB\.mkv')
    $seg = [regex]::Match($l, 'job: start yy_TB\.mp4[\s\S]*?job: done yy_TB\.mkv').Value
    Step 'Y: log: remux verified (> 90%) before the MP4 was deleted' ($seg -match 'job: MKV verification \(remux\): exists, \d+ bytes = [\d.]+% of the MP4''s \d+ bytes \(> 90%\) -- ok[\s\S]*job: deleted yy_TB\.mp4 \(keep_mp4 = 0\)[\s\S]*job: deleted yy_TB\.json with the MP4')
    Step 'Y: log: "already in the YouTube 8K layout -- 8K step skipped", no 8K command' ($seg -match 'job: recorded in the YouTube layout \(tb_half, 16:9, 768x432\): already in the YouTube 8K layout -- 8K step skipped' -and $seg -notmatch 'job: 8K cmd')
    Step 'Y: log: flat step takes the top eye and un-squeezes it' ($seg -match 'job: flat cmd \(libx264\): .*-vf "crop=iw:ih/2:0:0,scale=iw:ih\*2,setsar=1,crop=iw:min\(ih\\,iw\*9/16\),scale=3840:-2"')
    Step 'Y: balloon "Ready for YouTube: yy_TB.mkv" (+ the flat file)' ($l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube: yy_TB\.mkv\s+Flat 2D: yy_TB_FLAT\.mp4')

    # ---- J..L: eye_crop on TALL eyes (640x900), 8K only (S = 432), tb_half, MP4 kept
    # $vx / $vw: 4:3pad's picture inside each eye (the rest are black bars); 0 = the whole width
    function CropCase($name, $key, $crop, $w, $h, $cropLog, $vx = 0, $vw = 0) {
        Write-Host "`n=== ${name}: eye_crop = $crop (640x900 eyes, tb_half, short side 432)" -ForegroundColor Cyan
        WriteConfig 0 1 1 'tb_half' $crop
        $src = $tc[$key]
        Step "${name}: job ended" (RunJob $src)
        $tag = switch ($crop) { '4:3' { '_4x3' } '4:3pad' { '_4x3pad' } 'none' { '_full' } default { '' } }   # non-default crops are tagged in the name
        $y = [IO.Path]::ChangeExtension($src, $null).TrimEnd('.') + "_YT8K$tag.mkv"
        Step "${name}: _YT8K$tag.mkv made" (Test-Path $y)
        if (Test-Path $y) {
            $s = Probe $y 'stream=codec_name,width,height:stream_tags=stereo_mode'
            Write-Host ("  {0}x{1} stereo_mode={2}" -f $s.width, $s.height, $s.'TAG:stereo_mode')
            Step "${name}: frame ${w}x${h}, top_bottom" ([int]$s.width -eq $w -and [int]$s.height -eq $h -and $s.'TAG:stereo_mode' -eq 'top_bottom')
            $hh = [int]($h / 2)
            if ($vw -le 0) { $vw = $w }
            $oR = Avg $y ("{0}:{1}:{2}:{3}" -f ($vw - 4), $hh, ($vx + 2), $hh)   # the picture, 2 px in from a bar's (scaled) edge
            Step "${name}: bottom half = right eye (solid orange, no stretch artefacts)" (Near $oR @(200, 60, 30) 12)
            if ($vx -gt 0) {
                $bl = Avg $y ("{0}:{1}:0:0" -f ($vx - 4), $h); $br = Avg $y ("{0}:{1}:{2}:0" -f ($vx - 4), $h, ($vx + $vw + 4))
                Step "${name}: black bars left and right of both eyes (left $(Fmt $bl), right $(Fmt $br))" ((Near $bl @(0, 0, 0) 6) -and (Near $br @(0, 0, 0) 6))
            }
        }
        $l = LogText
        Step "${name}: log shows the crop and the frame size" ($l -match [regex]::Escape("8K step: input 1280x900 = eyes 640x900; eye_crop $crop -> $cropLog") -and $l -match ("tb_half frame {0}x{1}" -f $w, $h))
    }
    CropCase 'J' 'c169' '16:9' 768 432 'centre-cropped to 640x360'
    CropCase 'K' 'c43' '4:3' 576 432 'centre-cropped to 640x480'
    CropCase 'K2' 'c43pad' '4:3pad' 768 432 'centre-cropped to 640x480, black bars left and right to 854x480 (picture 576 wide at x 96 of each 768-wide eye)' 96 576
    Step 'K2: pad filter (eye units, before the scale) in the logged command' ((LogText) -match ([regex]::Escape(',pad=w=max(iw\,2*trunc(ih*16/9/2+0.5)):h=ih:x=trunc((ow-iw)/4)*2:y=0:color=black,scale=') + '[^\r\n]*ytc43pad_SBS_YT8K_4x3pad\.mkv'))
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

    # ---- N: never overwrite + crop tag in the name. The same MP4 (keep_mp4 1) converted 16:9, then 4:3, then 4:3 again:
    # _YT8K.mkv, _YT8K_4x3.mkv and _YT8K_4x3 (2).mkv must all exist (8K only, short side 432).
    Write-Host "`n=== N: one MP4 converted 16:9, 4:3, 4:3 -> _YT8K.mkv, _YT8K_4x3.mkv, _YT8K_4x3 (2).mkv" -ForegroundColor Cyan
    $nsrc = Join-Path $c 'yn_SBS.mp4'; Copy-Item $nsrc0 $nsrc
    $n1 = Join-Path $c 'yn_SBS_YT8K.mkv'; $n2 = Join-Path $c 'yn_SBS_YT8K_4x3.mkv'; $n3 = Join-Path $c 'yn_SBS_YT8K_4x3 (2).mkv'
    WriteConfig 0 1 1 'tb_half' '16:9'
    Step 'N: job 1 (16:9) ended' (RunJob $nsrc)
    Step 'N: _YT8K.mkv made (no tag for 16:9)' ((Test-Path $n1) -and -not (Test-Path $n2))
    $n1size = if (Test-Path $n1) { (Get-Item $n1).Length } else { -1 }
    WriteConfig 0 1 1 'tb_half' '4:3'
    # a tray announces each file once per run; N converts the same file again, so the tray restarts before each new job
    Stop-Process -Id $p.Id; $p.WaitForExit(); $p = StartTray
    Step 'N: job 2 (4:3) ended' (RunJob $nsrc)
    Step 'N: _YT8K_4x3.mkv made next to the 16:9 file' ((Test-Path $n1) -and (Test-Path $n2) -and -not (Test-Path $n3))
    Step 'N: the 16:9 file was not touched' ((Test-Path $n1) -and (Get-Item $n1).Length -eq $n1size)
    $n2size = if (Test-Path $n2) { (Get-Item $n2).Length } else { -1 }
    Stop-Process -Id $p.Id; $p.WaitForExit(); $p = StartTray
    Step 'N: job 3 (4:3 again) ended' (RunJob $nsrc)
    Step 'N: _YT8K_4x3 (2).mkv made, the other two kept' ((Test-Path $n1) -and (Test-Path $n2) -and (Test-Path $n3) -and (Get-Item $n2).Length -eq $n2size)
    $l = LogText
    Step 'N: log: "exists -- writing ... instead"' ($l -match 'job: 8K output yn_SBS_YT8K_4x3\.mkv exists -- writing yn_SBS_YT8K_4x3 \(2\)\.mkv instead')
    Step 'N: log + balloon use the chosen name' ($l -match '8K step ok in .* -> yn_SBS_YT8K_4x3 \(2\)\.mkv' -and $l -match 'balloon \[OpenXR-EyeCapture\] Ready for YouTube \(8K\): yn_SBS_YT8K_4x3 \(2\)\.mkv')

    # ---- P: progress, pause / resume, cancel (WM_APP+5 / WM_APP+6 to the TEST tray's window only)
    function WaitFor($cond, $sec) { $until = (Get-Date).AddSeconds($sec); while (-not (& $cond) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 200 }; return [bool](& $cond) }
    function Size($f) { if (Test-Path -LiteralPath $f) { return (Get-Item -LiteralPath $f).Length } else { return -1 } }
    function PostTray($msg) {
        $w = [Yt8kTest.W]::TrayWindow([uint32]$p.Id)
        if ($w -eq [IntPtr]::Zero) { Write-Host '  test tray window not found' -ForegroundColor Red; return $false }
        return [Yt8kTest.W]::PostMessageW($w, [uint32]$msg, [IntPtr]::Zero, [IntPtr]::Zero)
    }
    $WM_PAUSE = 0x8005; $WM_CANCEL = 0x8006
    function Announce($file) { & (Join-Path $bin 'status_sim.exe') 'eyecap-yt8k-test' 1 $file | Out-Null }

    Write-Host "`n=== P1: 120 s source, post_mkv 1 (tb_half re-encode), keep_mp4 1: progress, pause 2 s, resume" -ForegroundColor Cyan
    WriteConfig 1 0 1 'tb_half'
    $before = JobsEnded
    Announce $p1
    $o1 = Join-Path $c 'yp1_SBS_full.mkv'
    Step 'P1: progress logged (MKV progress 25%)' (WaitFor { (LogText) -match 'job: MKV progress 25% \([\d.]+ of 1[12]\d\.\d s' } 90)
    Step 'P1: tooltip line with percent + ETA' ((LogText) -match 'tray: tooltip job line: converting yp1_SBS\.mp4 \(MKV\) \d+%( - about \d+ (s|min) left)?')
    Step 'P1: pause posted' (PostTray $WM_PAUSE)
    Step 'P1: ffmpeg SUSPENDED (paused by user)' (WaitFor { (LogText) -match 'job: ffmpeg \(pid \d+\) SUSPENDED \(paused by user\)' } 5)
    Start-Sleep -Milliseconds 700
    $o1p = "$o1.part"   # a step writes <final>.part and renames it only after the verification
    Step 'P1: no final-named .mkv while the step runs (only the .part)' (-not (Test-Path -LiteralPath $o1) -and (Size $o1p) -gt 0)
    $s1 = Size $o1p; Start-Sleep -Seconds 2; $s2 = Size $o1p
    Write-Host ("  partial .mkv.part {0} -> {1} bytes over 2 s while paused" -f $s1, $s2)
    Step 'P1: output does not grow for 2 s while paused' ($s1 -gt 0 -and $s1 -eq $s2)
    Step 'P1: tooltip line "paused: ..."' (WaitFor { (LogText) -match 'tray: tooltip job line: paused: yp1_SBS\.mp4 \(MKV\) \d+% -- Resume in the tray menu' } 3)
    Step 'P1: resume posted' (PostTray $WM_PAUSE)
    Step 'P1: resumed (resumed by user)' (WaitFor { (LogText) -match 'job: ffmpeg \(pid \d+\) resumed \(resumed by user\)' } 5)
    Step 'P1: output grows again' (WaitFor { (Size $o1p) -gt $s2 } 10)
    Step 'P1: job finished after the resume' (WaitFor { (JobsEnded) -gt $before } 240)
    $l = LogText
    Step 'P1: done, .mkv made (the .part renamed), MP4 kept' (($l -match 'job: done yp1_SBS_full\.mkv') -and (Test-Path $o1) -and -not (Test-Path -LiteralPath $o1p) -and (Test-Path $p1))
    $seg = [regex]::Match($l, 'job: start yp1_SBS\.mp4[\s\S]*?job: done yp1_SBS_full\.mkv').Value
    Step 'P1: progress log is not spammy (<= 3 progress lines in the job)' ($seg -and ([regex]::Matches($seg, 'job: MKV progress')).Count -le 3)

    Write-Host "`n=== P2: post_mkv 1, keep_mp4 0, a short job queued behind: pause, cancel while paused" -ForegroundColor Cyan
    WriteConfig 1 0 0 'tb_half'
    $o2 = Join-Path $c 'yp2_SBS_full.mkv'; $oq = Join-Path $c 'yq_SBS_full.mkv'
    $before = JobsEnded
    Announce $p2
    Step 'P2: job running, partial .mkv growing' (WaitFor { (LogText) -match 'job: MKV cmd \(libx265\): .*yp2_SBS_full\.mkv' -and (Size "$o2.part") -gt 0 } 60)
    Announce $pq   # queued behind P2
    Step 'P2: second job queued' (WaitFor { (LogText) -match 'tray: queued .*yq_SBS\.mp4 for conversion' } 10)
    Step 'P2: pause posted' (PostTray $WM_PAUSE)
    Step 'P2: SUSPENDED' (WaitFor { ([regex]::Matches((LogText), 'SUSPENDED \(paused by user\)')).Count -ge 2 } 5)
    $susp = [regex]::Matches((LogText), 'job: ffmpeg \(pid (\d+)\) SUSPENDED \(paused by user\)')
    $ffpid = [int]$susp[$susp.Count - 1].Groups[1].Value
    Step 'P2: cancel posted' (PostTray $WM_CANCEL)
    Step 'P2: "job: cancelled by user" logged' (WaitFor { (LogText) -match 'job: cancelled by user yp2_SBS\.mp4 during the MKV step \(deleted partial yp2_SBS_full\.mkv\.part; MP4 kept' } 15)
    $l = LogText
    Step 'P2: ffmpeg resumed before it was ended (no suspended ffmpeg left)' ($l -match "job: ffmpeg \(pid $ffpid\) resumed \(cancel\)")
    Step "P2: that ffmpeg (pid $ffpid) is gone" (WaitFor { -not (Get-Process -Id $ffpid -ErrorAction SilentlyContinue) } 5)
    Step 'P2: partial .mkv.part deleted (no .mkv made), MP4 kept although keep_mp4 = 0' (-not (Test-Path $o2) -and -not (Test-Path -LiteralPath "$o2.part") -and (Test-Path $p2) -and -not ($l -match 'job: deleted yp2_SBS\.mp4'))
    Step 'P2: balloon "Conversion cancelled: yp2_SBS.mp4"' (WaitFor { (LogText) -match 'tray: balloon \[OpenXR-EyeCapture\] Conversion cancelled: yp2_SBS\.mp4' } 5)
    Step 'P2: not a failure (no "job: FAILED yp2")' (-not ($l -match 'job: FAILED yp2'))
    Step 'P2: the queued job still converts' ((WaitFor { (LogText) -match 'job: done yq_SBS_full\.mkv' } 60) -and (Test-Path $oq) -and -not (Test-Path $pq))
    Start-Sleep -Milliseconds 800
    Step 'P2: cancel with no job running is a no-op' ((PostTray $WM_CANCEL) -and (WaitFor { (LogText) -match 'tray: cancel \(WM_APP\+6\): no conversion running' } 5))

    # ---- C: dome recording -> skipped
    Write-Host "`n=== C: _VR180.mp4 + vr180 sidecar, post_mkv 0 + post_yt8k 1" -ForegroundColor Cyan
    WriteConfig 0 1 1
    Step 'C: job ended' (RunJob $v)
    $l = LogText
    Step 'C: skipped, logged, no _YT8K.mkv, MP4 kept' ($l -match 'job: 8K step skipped: VR180 recordings are already full-frame' -and -not (Test-Path (Join-Path $c 'yc_VR180_YT8K.mkv')) -and (Test-Path $v))
    Step 'C: balloon "Nothing to convert"' ($l -match 'balloon \[Nothing to convert\] yc_VR180\.mp4\s+VR180 recordings are already full-frame')

    # ---- Q: yt8k_codec = av1 on this NO_NVENC tray: the av1_nvenc plan is logged, then the HEVC chain (libx265 here)
    Write-Host "`n=== Q: yt8k_codec av1, no NVENC -> av1_nvenc planned, falls back to HEVC (libx265)" -ForegroundColor Cyan
    WriteConfig 0 1 1 'tb_half' 'none' '100' 'av1'
    Step 'Q: job ended' (RunJob $qsrc)
    $y = Join-Path $c 'yq8_SBS_YT8K_full.mkv'
    Step 'Q: _YT8K_full.mkv made' (Test-Path $y)
    if (Test-Path $y) {
        $s = Probe $y 'stream=codec_name,width,height,pix_fmt:stream_tags=stereo_mode'
        Write-Host ("  {0}x{1} {2} {3} stereo_mode={4}" -f $s.width, $s.height, $s.codec_name, $s.pix_fmt, $s.'TAG:stereo_mode')
        Step 'Q: HEVC fallback output: hevc 768x432 yuv420p, stereo_mode top_bottom' ($s.codec_name -eq 'hevc' -and $s.width -eq '768' -and $s.height -eq '432' -and $s.pix_fmt -eq 'yuv420p' -and $s.'TAG:stereo_mode' -eq 'top_bottom')
    }
    $l = LogText
    Step 'Q: log: yt8k_codec av1 plan = av1_nvenc with the VBR maths of yt8k_bitrate_mbps 100, HEVC fallback named' ($l -match 'job: 8K step: yt8k_codec av1, yt8k_bitrate_mbps 100 -> av1_nvenc -c:v av1_nvenc -preset p5 -tune hq -multipass qres -tier 1 -highbitdepth 0 -rc vbr -b:v 100M -maxrate 133M -bufsize 200M \(fallback: hevc_nvenc -c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v 100M -maxrate 133M -bufsize 200M, then libx265 crf 18\)')
    Step 'Q: log: av1_nvenc not available -> hevc_nvenc (falling back to HEVC) -> not available -> libx265' ($l -match 'job: av1_nvenc not available -- using hevc_nvenc \(yt8k_codec av1: falling back to HEVC\)' -and $l -match 'job: hevc_nvenc not available -- using libx265' -and $l -match 'job: 8K cmd \(libx265\): .*yq8_SBS_YT8K_full_AV1\.mkv')
    Step 'Q: made by libx265 (not av1_nvenc): the _AV1 name is dropped (renamed to _YT8K_full.mkv)' ($l -match 'job: 8K step: made by libx265, not av1_nvenc -- renamed yq8_SBS_YT8K_full_AV1\.mkv to yq8_SBS_YT8K_full\.mkv' -and -not (Test-Path (Join-Path $c 'yq8_SBS_YT8K_full_AV1.mkv')))

    # ---- S: an AV1 source (what codec = av1 records): remuxed as AV1, 8K re-encoded from it
    $hasAom = [bool](& $ffmpeg -hide_banner -encoders 2>$null | Select-String -SimpleMatch 'libaom-av1')
    if (-not $hasAom) {
        Write-Host "`n=== S: skipped (this ffmpeg has no libaom-av1 encoder to make the AV1 source)" -ForegroundColor Yellow
    } else {
        Write-Host "`n=== S: AV1 source (libaom), post_mkv 1 sbs_full + none (remux) + post_yt8k 1" -ForegroundColor Cyan
        $ssrc = Join-Path $c 'ys_SBS.mp4'
        & $ffmpeg -hide_banner -loglevel error -y -i $sIn -c:v libaom-av1 -cpu-used 8 -row-mt 1 -crf 30 -b:v 0 -c:a copy $ssrc
        $sc = & $ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 $ssrc
        Step "S: AV1 source made ($sc)" ($sc -eq 'av1')
        WriteConfig 1 1 1 'sbs_full' 'none'
        Step 'S: job ended' (RunJob $ssrc)
        $sm = Join-Path $c 'ys_SBS.mkv'; $sy = Join-Path $c 'ys_SBS_YT8K_full.mkv'
        Step 'S: .mkv and _YT8K_full.mkv made' ((Test-Path $sm) -and (Test-Path $sy))
        if (Test-Path $sm) {
            $s = Probe $sm 'stream=codec_name,width,height:stream_tags=stereo_mode'
            Write-Host ("  mkv: {0}x{1} {2} stereo_mode={3}" -f $s.width, $s.height, $s.codec_name, $s.'TAG:stereo_mode')
            Step 'S: remux keeps AV1: av1 1280x360, stereo_mode left_right' ($s.codec_name -eq 'av1' -and $s.width -eq '1280' -and $s.height -eq '360' -and $s.'TAG:stereo_mode' -eq 'left_right')
        }
        if (Test-Path $sy) {
            $s = Probe $sy 'stream=codec_name,width,height:stream_tags=stereo_mode'
            Write-Host ("  8K: {0}x{1} {2} stereo_mode={3}" -f $s.width, $s.height, $s.codec_name, $s.'TAG:stereo_mode')
            Step 'S: 8K from the AV1 source (layout sbs_full): hevc 1536x432 left_right' ($s.codec_name -eq 'hevc' -and $s.width -eq '1536' -and $s.height -eq '432' -and $s.'TAG:stereo_mode' -eq 'left_right')
        }
        $l = LogText
        Step 'S: log: source video is AV1; the software (libx265) command decodes without -hwaccel' ($l -match 'job: source video is AV1' -and $l -match 'job: 8K cmd \(libx265\): .* -y -i "[^"]*ys_SBS\.mp4"')
    }

    Write-Host "`n--- tray.log (job lines):"
    (LogText) -split "`r?`n" | Where-Object { $_ -match 'job: |balloon' } | ForEach-Object { Write-Host "  $_" }
    Step 'no job FAILED' (-not ((LogText) -match 'job: FAILED'))

    # ---- R: NVENC "available" (fake_ffmpeg lists hevc_nvenc + av1_nvenc and copies the input; no GPU is touched).
    # A second test tray WITHOUT EYECAP_TRAY_TEST_NO_NVENC; ffmpeg_path = the fake. R1: av1_nvenc runs. R2: av1_nvenc fails -> hevc_nvenc.
    Write-Host "`n=== R: fake ffmpeg with av1_nvenc listed: R1 av1_nvenc command, R2 av1_nvenc fails -> hevc_nvenc" -ForegroundColor Cyan
    Stop-Process -Id $p.Id; $p.WaitForExit()
    $fakeDir = Join-Path $t 'ff'; New-Item -ItemType Directory -Force $fakeDir | Out-Null
    $fake = Join-Path $fakeDir 'ffmpeg.exe'; Copy-Item (Join-Path $bin 'fake_ffmpeg.exe') $fake
    WriteConfig 0 1 1 'tb_half' 'none' '150' 'av1' $fake
    $env:FAKE_FFMPEG_ENCODERS = 'hevc_nvenc,av1_nvenc'; $env:FAKE_FFMPEG_MS = '200'
    $p = StartTray -Nvenc
    Step 'R1: job ended' (RunJob $r1)
    $l = LogText
    Step 'R1: 8K cmd (av1_nvenc) with the AV1 options and VBR maths' ($l -match 'job: 8K cmd \(av1_nvenc\): "[^"]*ff\\ffmpeg\.exe" .*-c:v av1_nvenc -preset p5 -tune hq -multipass qres -tier 1 -highbitdepth 0 -rc vbr -b:v 150M -maxrate 200M -bufsize 300M -c:a copy .*stereo_mode=top_bottom .*yr1_SBS_YT8K_full_AV1\.mkv')
    Step 'R1: no HEVC command for yr1, step ok, the file is named _AV1' (-not ($l -match 'job: 8K cmd \(hevc_nvenc\): .*yr1_SBS') -and $l -match 'job: 8K step ok in .* -> yr1_SBS_YT8K_full_AV1\.mkv' -and (Test-Path (Join-Path $c 'yr1_SBS_YT8K_full_AV1.mkv')))
    Stop-Process -Id $p.Id; $p.WaitForExit()
    $env:FAKE_FFMPEG_FAIL_ON = 'av1_nvenc'
    $p = StartTray -Nvenc
    Step 'R2: job ended' (RunJob $r2)
    $l = LogText
    Step 'R2: av1_nvenc failed -> retrying with hevc_nvenc (falling back to HEVC)' ($l -match 'job: 8K cmd \(av1_nvenc\): .*yr2_SBS_YT8K_full_AV1\.mkv' -and $l -match 'job: av1_nvenc failed \(fake_ffmpeg: av1_nvenc cannot be opened \(FAKE_FFMPEG_FAIL_ON\)\) -- retrying with hevc_nvenc \(yt8k_codec av1: falling back to HEVC\)')
    Step 'R2: then the hevc_nvenc command (same VBR maths), step ok, no job FAILED' ($l -match 'job: 8K cmd \(hevc_nvenc\): .*-c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v 150M -maxrate 200M -bufsize 300M .*yr2_SBS_YT8K_full_AV1\.mkv' -and $l -match 'job: 8K step ok in .* -> yr2_SBS_YT8K_full\.mkv' -and -not ($l -match 'job: FAILED'))
    Step 'R2: made by hevc_nvenc: renamed to the plain _YT8K_full.mkv (no _AV1 file left)' ($l -match 'job: 8K step: made by hevc_nvenc, not av1_nvenc -- renamed yr2_SBS_YT8K_full_AV1\.mkv to yr2_SBS_YT8K_full\.mkv' -and (Test-Path (Join-Path $c 'yr2_SBS_YT8K_full.mkv')) -and -not (Test-Path (Join-Path $c 'yr2_SBS_YT8K_full_AV1.mkv')))
} finally {
    $env:APPDATA = $oldAppdata
    foreach ($n in 'EYECAP_TRAY_TEST_SHM', 'EYECAP_TRAY_TEST_NO_NVENC', 'EYECAP_TRAY_TEST_YT8K_SHORT', 'FAKE_FFMPEG_ENCODERS', 'FAKE_FFMPEG_MS', 'FAKE_FFMPEG_FAIL_ON') { Remove-Item "Env:\$n" -ErrorAction SilentlyContinue }
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id }
}
if ($fail) { Write-Host "`n$fail check(s) FAILED" -ForegroundColor Red; exit 1 } else { Write-Host "`nall checks passed" -ForegroundColor Green; exit 0 }
