<#
    make_thumbnail.ps1 -- a YouTube thumbnail (1280 x 720 JPEG) from a recording: one frame of the LEFT eye, cropped to 16:9,
    with the project's overlay (docs\images\thumbnail_overlay.png: "3D" badge, VR EyeCapture logo, dark band) and
    the game's name as the title.

      powershell -ExecutionPolicy Bypass -File scripts\make_thumbnail.ps1 <recording.mp4|.mkv> [-Title "..."] [-Subtitle "..."]
                                                                        [-At 25% | -At 90 | -At 00:01:30] [-Out file.jpg]
                                                                        [-Layout auto|sbs_full|sbs_half|tb_full|tb_half] [-Eye left|right]

    Title: the game's name from the file name ("Beat Saber_20261016_220827_SBS.mp4" -> "Beat Saber") unless -Title says
    otherwise. Subtitle (one line under the title, grey) is empty unless given. -At picks the frame: a percentage of the
    duration (default 25%), seconds, or hh:mm:ss. Output: <recording>_thumb.jpg next to the recording.
    Layout "auto" reads the recording's .json sidecar (YouTube layout, dome recordings) or, for an .mkv, its stereo_mode
    tag: top_bottom = tb_half (the tray's default .mkv), left_right = sbs_full. A re-encoded sbs_half .mkv has no way to
    say so: pass -Layout sbs_half if the picture looks squeezed.
    Needs ffmpeg + ffprobe (winget install Gyan.FFmpeg.Essentials, or pass -Ffmpeg <path to ffmpeg.exe>).
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$File,
    [string]$Title = '',
    [string]$Subtitle = '',
    [string]$At = '25%',
    [string]$Out = '',
    [ValidateSet('auto', 'sbs_full', 'sbs_half', 'tb_full', 'tb_half')][string]$Layout = 'auto',
    [ValidateSet('left', 'right')][string]$Eye = 'left',
    [string]$Overlay = '',
    [string]$Ffmpeg = ''
)
$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Ffmpeg) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($cmd) { $Ffmpeg = $cmd.Source } else {
        $c = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter ffmpeg.exe -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($c) { $Ffmpeg = $c.FullName } else { Write-Host 'ffmpeg not found: winget install Gyan.FFmpeg.Essentials (or pass -Ffmpeg <path>)' -ForegroundColor Red; exit 1 }
    }
}
$Ffprobe = Join-Path (Split-Path -Parent $Ffmpeg) 'ffprobe.exe'
if (-not (Test-Path $Ffprobe)) { Write-Host "ffprobe not found next to $Ffmpeg" -ForegroundColor Red; exit 1 }
if (-not (Test-Path -LiteralPath $File)) { Write-Host "missing: $File" -ForegroundColor Red; exit 1 }
$File = (Resolve-Path -LiteralPath $File).Path

if (-not $Overlay) {
    foreach ($cand in @((Join-Path $scriptDir 'thumbnail_overlay.png'), (Join-Path $scriptDir '..\docs\images\thumbnail_overlay.png'))) {
        if (Test-Path -LiteralPath $cand) { $Overlay = (Resolve-Path -LiteralPath $cand).Path; break }
    }
    if (-not $Overlay) { Write-Host 'thumbnail_overlay.png not found (scripts\ or docs\images\); pass -Overlay <path>' -ForegroundColor Red; exit 1 }
}
$fontDir = Join-Path $env:WINDIR 'Fonts'
$fontBold = Join-Path $fontDir 'segoeuib.ttf'
$fontReg = Join-Path $fontDir 'segoeui.ttf'
foreach ($f in @($fontBold, $fontReg)) { if (-not (Test-Path $f)) { $fontBold = Join-Path $fontDir 'arialbd.ttf'; $fontReg = Join-Path $fontDir 'arial.ttf' } }

$stem = [System.IO.Path]::GetFileNameWithoutExtension($File)
$dir = Split-Path -Parent $File
if (-not $Title) {
    # "<game>_<yyyymmdd>_<hhmmss>_SBS[...]" -> "<game>"; anything else: the whole stem
    if ($stem -match '^(.*?)_\d{8}_\d{6}') { $Title = $Matches[1] } else { $Title = $stem }
    $Title = $Title -replace '_', ' '
}
if (-not $Out) { $Out = Join-Path $dir ($stem + '_thumb.jpg') }

# --- layout -------------------------------------------------------------------------------------------------------------
function Get-Sidecar([string]$path) {
    # the sidecar sits next to the MP4 as <recording>.json; an .mkv made from it carries suffixes after "_SBS"
    $s = [System.IO.Path]::GetFileNameWithoutExtension($path)
    $cands = @((Join-Path (Split-Path -Parent $path) ($s + '.json')))
    if ($s -match '^(.*?_SBS)') { $cands += (Join-Path (Split-Path -Parent $path) ($Matches[1] + '.json')) }
    foreach ($c in $cands) { if (Test-Path -LiteralPath $c) { return (Get-Content -LiteralPath $c -Raw) } }
    return $null
}
if ($Layout -eq 'auto') {
    $Layout = 'sbs_full'
    $side = Get-Sidecar $File
    if ($side) {
        if ($side -match '"packing"\s*:\s*"(sbs_full|sbs_half|tb_half)"') { $Layout = $Matches[1] }
        elseif ($side -match '"layout"\s*:\s*"top_bottom"') { $Layout = 'tb_full' }
    } elseif ([System.IO.Path]::GetExtension($File) -ieq '.mkv') {
        $tag = & $Ffprobe -v error -select_streams v:0 -show_entries stream_tags=stereo_mode -of csv=p=0 $File
        if ("$tag" -match 'top_bottom') { $Layout = 'tb_half' }
    }
}
# ffmpeg filter that turns the packed frame into one eye at its real proportions
$second = ($Eye -eq 'right')
if ($Layout -like 'sbs_*') {
    if ($second) { $eyeFilter = 'crop=iw/2:ih:iw/2:0' } else { $eyeFilter = 'crop=iw/2:ih:0:0' }
    if ($Layout -eq 'sbs_half') { $eyeFilter += ',scale=iw*2:ih' }
} else {
    if ($second) { $eyeFilter = 'crop=iw:ih/2:0:ih/2' } else { $eyeFilter = 'crop=iw:ih/2:0:0' }
    if ($Layout -eq 'tb_half') { $eyeFilter += ',scale=iw:ih*2' }
}

# --- time ---------------------------------------------------------------------------------------------------------------
$dur = & $Ffprobe -v error -show_entries format=duration -of csv=p=0 $File
$dur = [double]("$dur".Trim() -replace ',', '.')
if ($At -match '^\s*(\d+(\.\d+)?)\s*%\s*$') { $t = $dur * [double]$Matches[1] / 100.0 }
elseif ($At -match '^\d+(:\d+)*(\.\d+)?$' -and $At.Contains(':')) {
    $parts = $At -split ':'; $t = 0.0; foreach ($p in $parts) { $t = $t * 60 + [double]$p }
} else { $t = [double]$At }
if ($dur -gt 0 -and $t -ge $dur) { $t = [Math]::Max(0.0, $dur - 1.0) }
$tStr = $t.ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture)

# --- text ---------------------------------------------------------------------------------------------------------------
# drawtext reads the strings from files: no escaping of quotes, colons or backslashes in the title
function Esc-FilterPath([string]$p) { return "'" + (($p -replace '\\', '/') -replace ':', '\:') + "'" }
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('eyecapture_thumb_' + [System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$titleFile = Join-Path $tmp 'title.txt'
$subFile = Join-Path $tmp 'subtitle.txt'
$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($titleFile, $Title, $utf8)
[System.IO.File]::WriteAllText($subFile, $Subtitle, $utf8)

$textFilters = ",drawtext=fontfile=$(Esc-FilterPath $fontBold):textfile=$(Esc-FilterPath $titleFile):expansion=none:fontsize=92:fontcolor=0xf0f2f6:x=48:y=548:shadowcolor=black@0.55:shadowx=4:shadowy=4"
if ($Subtitle) {
    $textFilters += ",drawtext=fontfile=$(Esc-FilterPath $fontReg):textfile=$(Esc-FilterPath $subFile):expansion=none:fontsize=36:fontcolor=0x9aa3b2:x=50:y=656:shadowcolor=black@0.55:shadowx=3:shadowy=3"
}
$graph = "[0:v]$eyeFilter,scale=1280:720:force_original_aspect_ratio=increase,crop=1280:720,setsar=1[bg];[bg][1:v]overlay=0:0$textFilters"

Write-Host "$File"
Write-Host "  layout $Layout, $Eye eye, frame at $tStr s of $($dur.ToString('0.#', [System.Globalization.CultureInfo]::InvariantCulture)) s, title `"$Title`""
$outPart = $Out + '.part'
& $Ffmpeg -hide_banner -loglevel error -y -ss $tStr -i $File -i $Overlay -filter_complex $graph -frames:v 1 -update 1 -q:v 2 -f image2 $outPart
$rc = $LASTEXITCODE
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
if ($rc -ne 0 -or -not (Test-Path -LiteralPath $outPart)) { Write-Host '  ffmpeg failed' -ForegroundColor Red; Remove-Item -LiteralPath $outPart -Force -ErrorAction SilentlyContinue; exit 1 }
Move-Item -LiteralPath $outPart -Destination $Out -Force
$kb = [Math]::Round((Get-Item -LiteralPath $Out).Length / 1024)
Write-Host "  -> $Out ($kb KB)" -ForegroundColor Green
exit 0
