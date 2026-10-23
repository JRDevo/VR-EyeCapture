<#
.SYNOPSIS
    DEV ONLY (not in the installer): the README screenshots of the app window (one per tab) and the tray menu.

.DESCRIPTION
    Starts a scratch copy of build\Release\EyeCaptureTray.exe in build\tt_shots with its own %APPDATA% and the config.ini
    of the last commit (git show HEAD:config.ini = the defaults, never the personal working copy), an empty fake Steam folder
    and its own status block, so the pictures show default settings and no private paths. Then:
      - opens the app window (WM_APP+3 to the tray's message window) and shows each tab (kMsgShowTabOf = WM_APP+30 with a
        control of that tab); the Video tab is taken with Projection = VR180 (selected, not saved) so the dome settings are
        not greyed;
      - opens the tray menu (WM_APP+1 with WM_RBUTTONUP, as a right-click on the icon) and captures it;
      - closes the scratch tray.
    Pictures: docs\images\settings_<tab>_v<version>.png (width 1200) and docs\images\tray_menu_v<version>.png (as is).
    The version comes from CMakeLists.txt. The window follows the Windows light / dark setting (dark looks best on GitHub).
    Build first. Windows PowerShell 5.1 compatible.  powershell -ExecutionPolicy Bypass -File scripts\make_screenshots.ps1
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cml = Get-Content -Raw (Join-Path $root 'CMakeLists.txt')
$version = [regex]::Match($cml, 'project\s*\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
$tag = 'v' + ($version -replace '\.', '')
$out = Join-Path $root 'docs\images'
$t = Join-Path $root 'build\tt_shots'

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class Shot {
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr lp);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    public static readonly IntPtr HWND_MESSAGE = new IntPtr(-3);
    // The top-level window of process `pid` with class `cls` (visible only), or the message-only window when cls is the tray's.
    public static IntPtr Find(uint pid, string cls, bool messageOnly) {
        if (messageOnly) {
            IntPtr h = IntPtr.Zero;
            while ((h = FindWindowExW(HWND_MESSAGE, h, cls, null)) != IntPtr.Zero) {
                uint p; GetWindowThreadProcessId(h, out p);
                if (p == pid) return h;
            }
            return IntPtr.Zero;
        }
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, lp) => {
            uint p; GetWindowThreadProcessId(h, out p);
            var sb = new StringBuilder(64); GetClassNameW(h, sb, 64);
            if (p == pid && sb.ToString() == cls && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // PrintWindow with PW_RENDERFULLCONTENT: the window's own pixels, even when another window covers it.
    public static void SaveWindow(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        using (var bmp = new Bitmap(r.R - r.L, r.B - r.T, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
            bmp.Save(path, ImageFormat.Png);
        }
    }
    // A screen grab of the window's rectangle (for the popup menu, which PrintWindow does not draw).
    public static void SaveScreen(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        using (var bmp = new Bitmap(r.R - r.L, r.B - r.T, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(r.L, r.T, 0, 0, bmp.Size);
            bmp.Save(path, ImageFormat.Png);
        }
    }
    public static void Resize(string src, string dst, int width) {
        using (var a = Image.FromFile(src)) {
            int h = (int)Math.Round(a.Height * (double)width / a.Width);
            using (var b = new Bitmap(width, h))
            using (var g = Graphics.FromImage(b)) {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
                g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                g.DrawImage(a, 0, 0, width, h);
                b.Save(dst, ImageFormat.Png);
            }
        }
    }
}
'@
[void][Shot]::SetProcessDpiAwarenessContext([IntPtr](-4))   # per-monitor v2: window rects in device pixels

$exe = Join-Path $root 'build\Release\EyeCaptureTray.exe'
if (-not (Test-Path $exe)) { throw "build first: $exe is missing" }
Get-Process EyeCaptureTray -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$t*" } | Stop-Process
if (Test-Path $t) { Remove-Item -Recurse -Force $t }
New-Item -ItemType Directory (Join-Path $t 'appdata\VR EyeCapture'), (Join-Path $t 'steam'), (Join-Path $t 'raw') | Out-Null
Copy-Item $exe $t
$ini = (& git -C $root show HEAD:config.ini) -join "`r`n"
Set-Content (Join-Path $t 'appdata\VR EyeCapture\config.ini') $ini -Encoding utf8

$oldAppdata = $env:APPDATA
$p = $null
try {
    $env:APPDATA = Join-Path $t 'appdata'
    $env:EYECAP_TRAY_TEST_SHM = 'eyecap-screenshots'
    $env:EYECAP_TRAY_TEST_STEAM_DIR = Join-Path $t 'steam'
    $p = Start-Process (Join-Path $t 'EyeCaptureTray.exe') -PassThru
    Remove-Item Env:\EYECAP_TRAY_TEST_SHM, Env:\EYECAP_TRAY_TEST_STEAM_DIR
    $env:APPDATA = $oldAppdata
    $msg = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $msg -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $msg = [Shot]::Find([uint32]$p.Id, 'OpenXREyeCaptureTray', $true) }
    if ($msg -eq [IntPtr]::Zero) { throw 'the scratch tray has no message window' }

    # ---- the app window, one picture per tab (a control of each tab: Hotkey, Codec, Keep MP4, Log level)
    [void][Shot]::PostMessageW($msg, 0x8003, [IntPtr]::Zero, [IntPtr]::Zero)
    $dlg = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $dlg -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $dlg = [Shot]::Find([uint32]$p.Id, 'EyeCaptureSettings', $false) }
    if ($dlg -eq [IntPtr]::Zero) { throw 'the app window did not open' }
    Start-Sleep -Milliseconds 800
    $tabs = [ordered]@{ recording = 2001; export = 2023; app = 2026; video = 2003 }
    foreach ($name in $tabs.Keys) {
        if ($name -eq 'video') {   # Projection -> VR180 dome (item 1), as if picked: the dome row is live (nothing is saved)
            $pc = [Shot]::GetDlgItem($dlg, 2010)
            [void][Shot]::SendMessageW($pc, 0x014E, [IntPtr]1, [IntPtr]::Zero)                               # CB_SETCURSEL
            [void][Shot]::SendMessageW($dlg, 0x0111, [IntPtr]((1 -shl 16) -bor 2010), $pc)                    # WM_COMMAND CBN_SELCHANGE
        }
        [void][Shot]::SendMessageW($dlg, 0x8000 + 30, [IntPtr]$tabs[$name], [IntPtr]::Zero)
        Start-Sleep -Milliseconds 600
        $raw = Join-Path $t "raw\settings_$name.png"
        [Shot]::SaveWindow($dlg, $raw)
        [Shot]::Resize($raw, (Join-Path $out "settings_${name}_$tag.png"), 1200)
        Write-Host "settings_${name}_$tag.png"
    }
    $pc = [Shot]::GetDlgItem($dlg, 2010)   # Projection back to the file's value: no unsaved change, Exit does not ask
    [void][Shot]::SendMessageW($pc, 0x014E, [IntPtr]0, [IntPtr]::Zero)
    [void][Shot]::SendMessageW($dlg, 0x0111, [IntPtr]((1 -shl 16) -bor 2010), $pc)
    [void][Shot]::PostMessageW($dlg, 0x0111, [IntPtr]2028, [IntPtr]::Zero)   # Exit
    Start-Sleep -Milliseconds 800

    # ---- the tray menu (right-click on the icon)
    [void][Shot]::PostMessageW($msg, 0x8001, [IntPtr]::Zero, [IntPtr]0x0205)   # WM_TRAY, WM_RBUTTONUP
    $menu = [IntPtr]::Zero
    for ($i = 0; $i -lt 30 -and $menu -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $menu = [Shot]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, '#32768', $null) }
    if ($menu -eq [IntPtr]::Zero) { throw 'the tray menu did not open' }
    Start-Sleep -Milliseconds 500   # the menu fades in
    [Shot]::SaveScreen($menu, (Join-Path $out "tray_menu_$tag.png"))
    Write-Host "tray_menu_$tag.png"
    [void][Shot]::PostMessageW($menu, 0x0100, [IntPtr]0x1B, [IntPtr]::Zero)   # Esc closes the menu
} finally {
    $env:APPDATA = $oldAppdata
    if ($p -and -not $p.HasExited) {
        if ($msg -and $msg -ne [IntPtr]::Zero) { [void][Shot]::PostMessageW($msg, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }   # WM_CLOSE
        Start-Sleep -Seconds 2
        if (-not $p.HasExited) { Stop-Process -Id $p.Id }
    }
}
