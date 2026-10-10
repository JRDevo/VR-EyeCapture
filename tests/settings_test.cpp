// settings_test -- tests for the tray's Settings window (tray/settings.cpp). No GPU, no game.
//
//   settings_test hotkeys
//       every key name the window can produce round-trips through ParseHotkey (with every modifier combination).
//   settings_test save <config.ini>
//       Save path on a REAL file: bitrate 20, projection vr360, hotkey Ctrl+Alt+R written in place; bad values
//       (output_fov_scale 7, hotkey Ctrl+Banana) must be refused and leave the file byte-identical; a key from a
//       section that does not exist yet is appended. Prints what was written; tests/run_settings.ps1 diffs the file.
//   settings_test drive <tray-pid> [noslider]
//       drives the REAL window of a scratch tray (EYECAP_TRAY_TEST_SHM set): posts the "open settings" message, sets
//       controls with SendMessage, clicks Save. First with a bad value (the window must stay open, the tray logs the
//       message it would show), then good values (the window closes).
// Exit code 0 = all checks passed.
#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "config.h"
#include "settings.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}
std::string ReadAll(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
std::string N(const std::wstring& w) { return elog::Narrow(w); }

int TestHotkeys() {
    printf("hotkey names round-trip through ParseHotkey\n");
    int tried = 0, bad = 0;
    for (unsigned vk = 1; vk < 256; ++vk) {
        std::wstring t;
        if (!settings::HotkeyText(vk, false, false, false, t)) continue;
        for (int mods = 0; mods < 8; ++mods) {
            const bool c = mods & 1, s = mods & 2, a = mods & 4;
            settings::HotkeyText(vk, c, s, a, t);
            Hotkey h;
            ++tried;
            if (!ParseHotkey(t, h) || h.vk != vk || h.ctrl != c || h.shift != s || h.alt != a) {
                ++bad;
                printf("    mismatch: vk %u -> '%s'\n", vk, N(t).c_str());
            }
        }
    }
    printf("  %d hotkey texts tried\n", tried);
    Check(tried > 400 && bad == 0, "all produced hotkey texts parse back to the same key and modifiers");
    std::wstring t;
    settings::HotkeyText(VK_F10, true, true, false, t);
    Check(t == L"Ctrl+Shift+F10", "Ctrl+Shift+F10 text");
    Check(!settings::HotkeyText(VK_ESCAPE, false, false, false, t), "Escape has no config name");
    return g_fail;
}

int TestSave(const std::wstring& path) {
    printf("save path on %s\n", N(path).c_str());
    std::vector<std::wstring> problems;
    Config c;
    ParseConfigFile(path, c, problems);
    Check(problems.empty(), "the scratch config parses without problems");
    const std::string original = ReadAll(path);

    // 1. bad values: refused, file untouched
    {
        Config bad = c;
        bad.head.fovScale = 7.0f;
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(path, bad, r);
        printf("  output_fov_scale = 7 -> ok=%d, problems:\n", ok);
        for (const auto& p : r.problems) printf("      %s\n", N(p).c_str());
        Check(!ok && !r.problems.empty() && ReadAll(path) == original, "output_fov_scale = 7 is refused and the file is unchanged");
    }
    {
        Config bad = c;
        ParseHotkey(L"Ctrl+Shift+F10", bad.hotkey);
        bad.hotkey.text = L"Ctrl+Banana";
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(path, bad, r);
        printf("  hotkey = Ctrl+Banana -> ok=%d, problems:\n", ok);
        for (const auto& p : r.problems) printf("      %s\n", N(p).c_str());
        Check(!ok && !r.problems.empty() && ReadAll(path) == original, "hotkey Ctrl+Banana is refused and the file is unchanged");
    }
    // 2. nothing changed: no write at all
    {
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(path, c, r);
        Check(ok && r.changed.empty() && ReadAll(path) == original, "saving unchanged values changes nothing");
    }
    // 3. three real changes
    {
        Config n = c;
        n.bitrateMbps = 20;
        n.dome.projection = DomeProjection::Vr360;
        ParseHotkey(L"Ctrl+Alt+R", n.hotkey);
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(path, n, r);
        printf("  changed:");
        for (const auto& s : r.changed) printf(" [%s]", N(s).c_str());
        printf("\n");
        Check(ok && r.changed.size() == 3, "exactly three keys reported as changed");
        Config back;
        std::vector<std::wstring> pr;
        ParseConfigFile(path, back, pr);
        Check(pr.empty() && back.bitrateMbps == 20 && back.dome.projection == DomeProjection::Vr360 &&
                  back.hotkey.vk == 'R' && back.hotkey.ctrl && back.hotkey.alt && !back.hotkey.shift,
              "re-reading the file gives the new bitrate, projection and hotkey");
    }
    // 4. a key whose section is missing is appended (new file, so nothing else is touched)
    {
        const std::wstring p2 = path + L".append-test.ini";
        { std::ofstream f(p2, std::ios::binary); f << "; tiny config\r\n[capture]\r\ncodec = hevc\r\n"; }
        Config n;
        ParseHotkey(L"Ctrl+Shift+F10", n.hotkey);
        n.indicator.size = 0.05f;      // [indicator] does not exist
        n.bitrateMbps = 33;            // [capture] exists
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(p2, n, r);
        const std::string t = ReadAll(p2);
        printf("  appended file:\n%s\n", t.c_str());
        Check(ok && r.changed.size() == 2, "two keys appended");
        Check(t == "; tiny config\r\n[capture]\r\ncodec = hevc\r\nbitrate_mbps = 33\r\n\r\n[indicator]\r\nindicator_size = 0.05\r\n",
              "bitrate_mbps went to the end of [capture]; [indicator] was created at the end; CRLF kept");
        DeleteFileW(p2.c_str());
    }
    // 5. indicator position, the 0.0005 size minimum and log_level = none
    {
        const std::wstring p3 = path + L".indicator-test.ini";
        { std::ofstream f(p3, std::ios::binary); f << "[indicator]\r\nvr_indicator = 1\r\n[debug]\r\nlog_level = info\r\n"; }
        Config n;
        ParseHotkey(L"Ctrl+Shift+F10", n.hotkey);
        n.indicator.x = -0.3f; n.indicator.y = 0.05f; n.indicator.z = -1.5f;
        n.indicator.size = 0.0005f;
        n.logLevel = LogLevel::None;
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(p3, n, r);
        Config back;
        std::vector<std::wstring> pr;
        ParseConfigFile(p3, back, pr);
        printf("  indicator/log file:\n%s\n", ReadAll(p3).c_str());
        Check(ok && pr.empty() && back.indicator.x == -0.3f && back.indicator.y == 0.05f && back.indicator.z == -1.5f &&
                  back.indicator.size == 0.0005f && back.logLevel == LogLevel::None,
              "indicator_x/y/z, indicator_size 0.0005 and log_level = none are saved and read back");
        Config tiny = n;
        tiny.indicator.size = 0.0004f;
        settings::SaveResult r2;
        const bool ok2 = settings::SaveSettingsToFile(p3, tiny, r2);
        Check(!ok2 && !r2.problems.empty(), "indicator_size 0.0004 (below the minimum) is refused");
        Config off;
        { std::ofstream f(p3, std::ios::binary); f << "[debug]\r\nlog_level = off\r\n"; }
        ParseConfigFile(p3, off, pr);
        Check(off.logLevel == LogLevel::None, "log_level = off reads as none");
        DeleteFileW(p3.c_str());
    }
    return g_fail;
}

struct FindCtx { DWORD pid; HWND found; };
BOOL CALLBACK FindDlg(HWND h, LPARAM lp) {
    auto* c = (FindCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t t[128];
    GetWindowTextW(h, t, 128);
    if (pid == c->pid && wcscmp(t, settings::kWindowTitle) == 0) { c->found = h; return FALSE; }
    return TRUE;
}
HWND FindDialog(DWORD pid) {
    FindCtx c{pid, nullptr};
    EnumWindows(FindDlg, (LPARAM)&c);
    return c.found;
}
HWND WaitDialog(DWORD pid, bool wantOpen, int ms = 5000) {
    for (int i = 0; i < ms / 50; ++i) {
        HWND d = FindDialog(pid);
        if ((d != nullptr) == wantOpen) return d;
        Sleep(50);
    }
    return FindDialog(pid);
}
void SetEdit(HWND d, int id, const wchar_t* t) { SendMessageW(GetDlgItem(d, id), WM_SETTEXT, 0, (LPARAM)t); }
std::wstring GetEdit(HWND d, int id) {
    wchar_t b[512] = L"";
    SendMessageW(GetDlgItem(d, id), WM_GETTEXT, 512, (LPARAM)b);
    return b;
}

int TestDrive(DWORD pid, bool leaveSlider) {
    printf("drive the Settings window of tray pid %lu\n", pid);
    FindCtx c{pid, nullptr};
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, L"OpenXREyeCaptureTray", nullptr)) != nullptr;) {
        DWORD wp = 0;
        GetWindowThreadProcessId(w, &wp);
        if (wp == pid) { c.found = w; break; }   // never the user's own tray
    }
    Check(c.found != nullptr, "found the tray's message window");
    if (!c.found) return 1;
    PostMessageW(c.found, WM_APP + 3, 0, 0);
    HWND d = WaitDialog(pid, true);
    Check(d != nullptr, "Settings window opened");
    if (!d) return 1;
    printf("  window title '%s', hotkey field '%s', bitrate '%s'\n", "OpenXR-EyeCapture Settings", N(GetEdit(d, settings::ID_HOTKEY)).c_str(),
           N(GetEdit(d, settings::ID_BITRATE)).c_str());

    // "Press a key": F7 without modifiers
    SendMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), BM_CLICK, 0, 0);
    PostMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), WM_KEYDOWN, VK_F7, 0);
    Sleep(300);
    Check(GetEdit(d, settings::ID_HOTKEY) == L"F7", "'Press a key...' captured F7 into the hotkey field");

    // bad value: Save must refuse and keep the window open
    SetEdit(d, settings::ID_FOVSCALE, L"7");
    SendMessageW(GetDlgItem(d, settings::ID_SAVE), BM_CLICK, 0, 0);
    Sleep(300);
    Check(FindDialog(pid) != nullptr, "bad output_fov_scale: Save refused, window stays open (message logged by the tray)");

    // good values
    SetEdit(d, settings::ID_FOVSCALE, L"0.85");
    SetEdit(d, settings::ID_BITRATE, L"20");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 0, 0);          // flat
    SendMessageW(GetDlgItem(d, settings::ID_SWAP), BM_SETCHECK, BST_CHECKED, 0);
    if (!leaveSlider) SendMessageW(GetDlgItem(d, settings::ID_SMOOTH), TBM_SETPOS, TRUE, 7);   // 0.35 ("noslider": untouched keeps the file text)
    SendMessageW(GetDlgItem(d, settings::ID_SAVE), BM_CLICK, 0, 0);
    Check(WaitDialog(pid, false) == nullptr, "good values: Save closed the window");
    return g_fail;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"hotkeys") == 0) return TestHotkeys() ? 1 : 0;
    if (argc >= 3 && wcscmp(argv[1], L"save") == 0) return TestSave(argv[2]) ? 1 : 0;
    if (argc >= 3 && wcscmp(argv[1], L"drive") == 0) return TestDrive((DWORD)_wtoi(argv[2]), argc >= 4 && wcscmp(argv[3], L"noslider") == 0) ? 1 : 0;
    printf("usage: settings_test hotkeys | save <config.ini> | drive <tray pid> [noslider]\n");
    return 2;
}
