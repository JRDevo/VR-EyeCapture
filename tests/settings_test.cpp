// settings_test -- tests for the tray's Settings window (tray/settings.cpp). No GPU, no game.
//
//   settings_test hotkeys
//       every key name the window can produce round-trips through ParseHotkey (with every modifier combination), plus
//       mouse / media / generic Key0xNN names, Pad: and Joy: bindings, binding lists and refused inputs (src/hotkey.h).
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
#include "install.h"
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
            if (!ParseHotkey(t, h) || h.vk != vk || h.ctrl != c || h.shift != s || h.alt != a || h.text != t ||
                h.bindings.size() != 1) {
                ++bad;
                printf("    mismatch: vk %u -> '%s'\n", vk, N(t).c_str());
            }
        }
    }
    // every VK 1..255 except Escape and the 11 modifier / Windows keys (Shift Ctrl Alt, L/R variants, LWin RWin) = 243
    printf("  %d hotkey texts tried (243 keys x 8 modifier sets = 1944 expected)\n", tried);
    Check(tried == 1944 && bad == 0, "all produced hotkey texts parse back to the same key, modifiers and text");
    std::wstring t;
    settings::HotkeyText(VK_F10, true, true, false, t);
    Check(t == L"Ctrl+Shift+F10", "Ctrl+Shift+F10 text");
    Check(!settings::HotkeyText(VK_ESCAPE, false, false, false, t), "Escape has no config name");
    Check(!settings::HotkeyText(VK_LSHIFT, false, false, false, t) && !settings::HotkeyText(VK_LWIN, false, false, false, t),
          "bare modifier / Windows keys have no config name");
    settings::HotkeyText(VK_SCROLL, false, false, false, t);
    Check(t == L"ScrollLock", "ScrollLock text unchanged");

    // canonical texts that must round-trip exactly (parse -> text -> same string)
    struct RT { const wchar_t* text; size_t n; };
    const RT roundTrip[] = {
        {L"MouseX1", 1}, {L"MediaPlayPause", 1}, {L"Key0xE5", 1}, {L"Ctrl+Alt+Semicolon", 1}, {L"CapsLock", 1},
        {L"Pad:A", 1}, {L"Pad:Back+Start", 1}, {L"Pad2:LB+RB+Guide", 1}, {L"Joy:*:Button12", 1},
        {L"Joy:Thrustmaster T300RS Racing Wheel:Button5+Button6+POVUp", 1},
        {L"ScrollLock, Pad:Back+Start, Joy:*:Button1", 3},
    };
    for (const RT& r : roundTrip) {
        Hotkey h;
        const bool ok = ParseHotkey(r.text, h);
        printf("    '%s' -> %s '%s' (%zu bindings)\n", N(r.text).c_str(), ok ? "ok" : "REFUSED", N(h.text).c_str(), h.bindings.size());
        Check(ok && h.text == r.text && h.bindings.size() == r.n, (std::string("round-trip ") + N(r.text)).c_str());
    }
    {
        Hotkey h;
        const bool ok = ParseHotkey(L"ScrollLock, Pad:Back+Start, Joy:*:Button1", h);
        Check(ok && h.vk == VK_SCROLL && !h.ctrl && !h.shift && !h.alt && h.bindings[1].kind == HotkeyBinding::Kind::Pad &&
                  h.bindings[1].padButtons == (kPadBack | kPadStart) && h.bindings[2].kind == HotkeyBinding::Kind::Joy &&
                  h.bindings[2].joyDevice == L"*" && h.bindings[2].joyButtons[0] == 1u,
              "list: vk = ScrollLock (first Key binding), Pad Back+Start mask, Joy any device Button1");
        Hotkey p;
        Check(ParseHotkey(L"Pad:A", p) && p.vk == 0 && p.bindings.size() == 1, "a pad-only hotkey has vk 0 (layer polls nothing)");
        Hotkey j;
        Check(ParseHotkey(L"joy: thrustmaster  t300rs :button5+povup+BUTTON6", j) &&
                  j.text == L"Joy:thrustmaster t300rs:Button5+Button6+POVUp" && j.bindings[0].joyPov == kPovUp,
              "Joy parsing: case-insensitive, spaces collapsed, buttons sorted, POV last");
        Hotkey k;
        Check(ParseHotkey(L"ctrl+shift+f10", k) && k.text == L"Ctrl+Shift+F10" && ParseHotkey(L"scrolllock", k) && k.text == L"ScrollLock" &&
                  ParseHotkey(L"key0x91", k) && k.text == L"ScrollLock" && ParseHotkey(L"pad:back+start", k) && k.text == L"Pad:Back+Start",
              "lower-case input gives the canonical spelling");
        Hotkey n;
        Check(ParseHotkey(L"none", n) && n.bindings.empty() && n.text == L"none" && n.vk == 0, "none = no bindings");
    }
    for (const wchar_t* bad : {L"Pad:Banana", L"Joy:x", L"Joy:x:Button0", L"Joy:x:Button129", L"Pad5:A", L"F7, ", L"Ctrl+Banana",
                               L"Joy::Button1", L"Pad:", L"Key0x1B", L"none, F7", L""}) {
        Hotkey h;
        h.text = L"untouched";
        const bool ok = ParseHotkey(bad, h);
        Check(!ok && h.text == L"untouched", (std::string("refused: '") + N(bad) + "'").c_str());
    }
    Check(SanitizeDeviceName(L"  Foo, Bar:Baz+Qux  #2 ") == L"Foo Bar Baz Qux 2" && JoyDeviceMatches(L"bar baz", L"Foo Bar Baz Qux 2") &&
              !JoyDeviceMatches(L"T300", L"Foo Bar Baz Qux 2") && JoyDeviceMatches(L"*", L"anything"),
          "device names: sanitised, substring match ignores case, * matches any");
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
    // 6. layout_3d: each value is saved and read back; an unknown value in the file reads as tb_half with a problem
    {
        const std::wstring p4 = path + L".yt8k-test.ini";
        { std::ofstream f(p4, std::ios::binary); f << "[post_processing]\r\npost_yt8k = 1\r\nlayout_3d = tb_half\r\n"; }
        const Layout3d all[] = {Layout3d::SbsHalf, Layout3d::SbsFull, Layout3d::TbFull, Layout3d::TbHalf};
        const char* names[] = {"sbs_half", "sbs_full", "tb_full", "tb_half"};
        for (int i = 0; i < 4; ++i) {
            Config n;
            ParseHotkey(L"Ctrl+Shift+F10", n.hotkey);
            n.postYt8k = true;
            n.layout3d = all[i];
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p4, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(p4, back, pr);
            Check(ok && pr.empty() && back.layout3d == all[i] && ReadAll(p4).find(std::string("layout_3d = ") + names[i]) != std::string::npos,
                  (std::string("layout_3d = ") + names[i] + " is saved and read back").c_str());
        }
        { std::ofstream f(p4, std::ios::binary); f << "[post_processing]\r\nlayout_3d = Sbs_Full\r\n"; }
        Config ci;
        std::vector<std::wstring> pr;
        ParseConfigFile(p4, ci, pr);
        Check(pr.empty() && ci.layout3d == Layout3d::SbsFull, "layout_3d is case-insensitive (Sbs_Full)");
        { std::ofstream f(p4, std::ios::binary); f << "[post_processing]\r\nlayout_3d = sideways\r\n"; }
        Config bad;
        std::vector<std::wstring> pr2;
        ParseConfigFile(p4, bad, pr2);
        Check(pr2.size() == 1 && bad.layout3d == Layout3d::TbHalf, "layout_3d = sideways: reported as a bad value, tb_half kept");
        DeleteFileW(p4.c_str());
    }
    // 7. post_when: default game_exit; each value is saved and read back; case-insensitive; a bad value keeps game_exit
    {
        Config def;
        Check(def.postWhen == PostWhen::GameExit, "post_when defaults to game_exit");
        const std::wstring p5 = path + L".postwhen-test.ini";
        { std::ofstream f(p5, std::ios::binary); f << "[post_processing]\r\npost_mkv = 1\r\npost_when = game_exit\r\n"; }
        const PostWhen all[] = {PostWhen::Now, PostWhen::SessionEnd, PostWhen::GameExit};
        const char* names[] = {"now", "session_end", "game_exit"};
        for (int i = 0; i < 3; ++i) {
            Config n;
            ParseHotkey(L"Ctrl+Shift+F10", n.hotkey);
            n.postWhen = all[i];
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p5, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(p5, back, pr);
            const std::string text = ReadAll(p5);
            Check(ok && pr.empty() && back.postWhen == all[i] && text.find(std::string("post_when = ") + names[i]) != std::string::npos &&
                      text.find("post_when", text.find("post_when") + 1) == std::string::npos,
                  (std::string("post_when = ") + names[i] + " is saved in place and read back").c_str());
        }
        { std::ofstream f(p5, std::ios::binary); f << "[post_processing]\r\npost_when = Session_End\r\n"; }
        Config ci;
        std::vector<std::wstring> pr;
        ParseConfigFile(p5, ci, pr);
        Check(pr.empty() && ci.postWhen == PostWhen::SessionEnd, "post_when is case-insensitive (Session_End)");
        { std::ofstream f(p5, std::ios::binary); f << "[post_processing]\r\npost_when = later\r\n"; }
        Config bad;
        std::vector<std::wstring> pr2;
        ParseConfigFile(p5, bad, pr2);
        Check(pr2.size() == 1 && bad.postWhen == PostWhen::GameExit, "post_when = later: reported as a bad value, game_exit kept");
        // a file without the key: saving the default adds nothing, saving "now" appends it to [post_processing]
        { std::ofstream f(p5, std::ios::binary); f << "[post_processing]\r\npost_mkv = 1\r\n"; }
        Config dflt;
        ParseHotkey(L"Ctrl+Shift+F10", dflt.hotkey);
        dflt.postMkv = true;
        settings::SaveResult r1;
        const bool ok1 = settings::SaveSettingsToFile(p5, dflt, r1);
        Check(ok1 && ReadAll(p5) == "[post_processing]\r\npost_mkv = 1\r\n", "missing post_when + default game_exit: the file is left alone");
        dflt.postWhen = PostWhen::Now;
        settings::SaveResult r2;
        const bool ok2 = settings::SaveSettingsToFile(p5, dflt, r2);
        Check(ok2 && r2.changed.size() == 1 && ReadAll(p5) == "[post_processing]\r\npost_mkv = 1\r\npost_when = now\r\n",
              "missing post_when + now: appended to [post_processing]");
        DeleteFileW(p5.c_str());
    }
    // 8. eye_crop: default 16:9; each value saved in place and read back; case-insensitive; bad value keeps 16:9
    {
        Config def;
        Check(def.eyeCrop == EyeCrop::Wide169, "eye_crop defaults to 16:9");
        const std::wstring p6 = path + L".eyecrop-test.ini";
        { std::ofstream f(p6, std::ios::binary); f << "[post_processing]\r\npost_mkv = 1\r\neye_crop = 16:9\r\n"; }
        const EyeCrop all[] = {EyeCrop::Std43, EyeCrop::None, EyeCrop::Wide169};
        const char* names[] = {"4:3", "none", "16:9"};
        for (int i = 0; i < 3; ++i) {
            Config n;
            ParseHotkey(L"Ctrl+Shift+F10", n.hotkey);
            n.eyeCrop = all[i];
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p6, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(p6, back, pr);
            const std::string text = ReadAll(p6);
            Check(ok && pr.empty() && back.eyeCrop == all[i] && text.find(std::string("eye_crop = ") + names[i]) != std::string::npos &&
                      text.find("eye_crop", text.find("eye_crop") + 1) == std::string::npos,
                  (std::string("eye_crop = ") + names[i] + " is saved in place and read back").c_str());
        }
        { std::ofstream f(p6, std::ios::binary); f << "[post_processing]\r\neye_crop = None\r\n"; }
        Config ci;
        std::vector<std::wstring> pr;
        ParseConfigFile(p6, ci, pr);
        Check(pr.empty() && ci.eyeCrop == EyeCrop::None, "eye_crop is case-insensitive (None)");
        { std::ofstream f(p6, std::ios::binary); f << "[post_processing]\r\neye_crop = 3:2\r\n"; }
        Config bad;
        std::vector<std::wstring> pr2;
        ParseConfigFile(p6, bad, pr2);
        Check(pr2.size() == 1 && bad.eyeCrop == EyeCrop::Wide169, "eye_crop = 3:2: reported as a bad value, 16:9 kept");
        DeleteFileW(p6.c_str());
    }
    // 9. record_overlays: default 1; a missing key + default leaves the file alone; 0 is appended to [capture] and read
    // back; changed in place; boolean spellings; a bad value is reported and keeps 1
    {
        Config def;
        Check(def.recordOverlays, "record_overlays defaults to 1");
        Check(c.recordOverlays, "the scratch copy of config.ini has record_overlays = 1");
        const std::wstring p7 = path + L".overlays-test.ini";
        { std::ofstream f(p7, std::ios::binary); f << "[capture]\r\nswap_eyes = 0\r\n\r\n[debug]\r\nlog_level = info\r\n"; }
        Config n;
        ParseHotkey(L"Ctrl+Shift+F10", n.hotkey);
        settings::SaveResult r1;
        const bool ok1 = settings::SaveSettingsToFile(p7, n, r1);
        Check(ok1 && ReadAll(p7) == "[capture]\r\nswap_eyes = 0\r\n\r\n[debug]\r\nlog_level = info\r\n",
              "missing record_overlays + default 1: the file is left alone");
        n.recordOverlays = false;
        settings::SaveResult r2;
        const bool ok2 = settings::SaveSettingsToFile(p7, n, r2);
        Config back;
        std::vector<std::wstring> pr;
        ParseConfigFile(p7, back, pr);
        Check(ok2 && r2.changed.size() == 1 && pr.empty() && !back.recordOverlays &&
                  ReadAll(p7) == "[capture]\r\nswap_eyes = 0\r\nrecord_overlays = 0\r\n\r\n[debug]\r\nlog_level = info\r\n",
              "record_overlays = 0: appended to the end of [capture] and read back as off");
        n.recordOverlays = true;
        settings::SaveResult r3;
        const bool ok3 = settings::SaveSettingsToFile(p7, n, r3);
        ParseConfigFile(p7, back, pr);
        Check(ok3 && r3.changed.size() == 1 && pr.empty() && back.recordOverlays &&
                  ReadAll(p7).find("record_overlays = 1\r\n") != std::string::npos,
              "record_overlays back to 1: changed in place");
        { std::ofstream f(p7, std::ios::binary); f << "[capture]\r\nrecord_overlays = off\r\n"; }
        Config o;
        ParseConfigFile(p7, o, pr);
        Check(pr.empty() && !o.recordOverlays, "record_overlays = off reads as 0");
        { std::ofstream f(p7, std::ios::binary); f << "[capture]\r\nrecord_overlays = maybe\r\n"; }
        Config bad;
        std::vector<std::wstring> pr2;
        ParseConfigFile(p7, bad, pr2);
        Check(pr2.size() == 1 && bad.recordOverlays, "record_overlays = maybe: reported as a bad value, 1 kept");
        DeleteFileW(p7.c_str());
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
    // the scratch tray (EYECAP_TRAY_TEST_SHM) keeps its "Start with Windows" entry in this test subkey, never the real Run key
    install::SetAutostartTestKey(L"Software\\OpenXR-EyeCapture-Test\\Run");
    { std::wstring e; install::SetAutostart(false, L"", e); }
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

    // "Add another...": the mouse side button X1 is appended to the list
    SendMessageW(GetDlgItem(d, settings::ID_HOTKEY_ADD), BM_CLICK, 0, 0);
    PostMessageW(d, WM_XBUTTONDOWN, (WPARAM)XBUTTON1 << 16, 0);
    Sleep(300);
    printf("    after Add another... + mouse X1: '%s'\n", N(GetEdit(d, settings::ID_HOTKEY)).c_str());
    Check(GetEdit(d, settings::ID_HOTKEY) == L"F7, MouseX1", "'Add another...' appended MouseX1: 'F7, MouseX1'");
    wchar_t btn[128] = L"";
    GetWindowTextW(GetDlgItem(d, settings::ID_HOTKEY_ADD), btn, 128);
    Check(wcscmp(btn, L"Add another...") == 0, "the Add button is disarmed again after the capture");

    // "Press a key..." again replaces the whole list
    SendMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), BM_CLICK, 0, 0);
    PostMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), WM_KEYDOWN, VK_F7, 0);
    Sleep(300);
    Check(GetEdit(d, settings::ID_HOTKEY) == L"F7", "'Press a key...' + F7 replaced the list: 'F7'");

    // bad value: Save must refuse and keep the window open
    SetEdit(d, settings::ID_FOVSCALE, L"7");
    SendMessageW(GetDlgItem(d, settings::ID_SAVE), BM_CLICK, 0, 0);
    Sleep(300);
    Check(FindDialog(pid) != nullptr, "bad output_fov_scale: Save refused, window stays open (message logged by the tray)");

    // good values
    SetEdit(d, settings::ID_FOVSCALE, L"0.85");
    SetEdit(d, settings::ID_BITRATE, L"20");
    // "3D layout" drop-down (3D group, under Projection): greyed unless Projection is flat. The scratch config starts as vr180.
    Check(!IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "3D layout drop-down is greyed while Projection is vr180 (loaded from the file)");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 0, 0);          // flat
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_PROJECTION));
    Check(IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "3D layout drop-down is enabled once Projection is changed to flat");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 2, 0);          // vr360
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_PROJECTION));
    Check(!IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "3D layout drop-down is greyed again for vr360");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 0, 0);          // flat
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_PROJECTION));
    Check(IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "3D layout drop-down is enabled for flat again");
    SendMessageW(GetDlgItem(d, settings::ID_SWAP), BM_SETCHECK, BST_CHECKED, 0);
    // "Record overlays" check (record_overlays): loaded checked from the file, unticked -> record_overlays = 0
    {
        const HWND ovc = GetDlgItem(d, settings::ID_OVERLAYS);
        wchar_t txt[96] = L"";
        GetWindowTextW(ovc, txt, 96);
        printf("    Record overlays check: '%s', %s\n", N(txt).c_str(),
               SendMessageW(ovc, BM_GETCHECK, 0, 0) == BST_CHECKED ? "checked" : "unchecked");
        Check(ovc != nullptr && IsWindowVisible(ovc) && SendMessageW(ovc, BM_GETCHECK, 0, 0) == BST_CHECKED &&
                  wcscmp(txt, L"Record overlays (mod menus, in-game panels)") == 0,
              "Record overlays check shows record_overlays = 1 from the file");
        SendMessageW(ovc, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    // "Make for YouTube" drop-down: item 2 = "both files" (post_mkv = 1 and post_yt8k = 1; run_settings.ps1 checks the saved file)
    SendMessageW(GetDlgItem(d, settings::ID_YTMODE), CB_SETCURSEL, 2, 0);
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_YTMODE, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_YTMODE));
    // "3D layout" drop-down: item 1 = side by side, half (layout_3d = sbs_half)
    SendMessageW(GetDlgItem(d, settings::ID_LAYOUT3D), CB_SETCURSEL, 1, 0);
    // "Convert recordings" drop-down (post_when): loaded as item 2 "When the game closes"; item 0 = "Right away" (now)
    {
        const HWND pw = GetDlgItem(d, settings::ID_POSTWHEN);
        wchar_t item[64] = L"";
        const int sel = (int)SendMessageW(pw, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(pw, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    Convert recordings drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(pw, CB_GETCOUNT, 0, 0), sel, N(item).c_str());
        Check(pw != nullptr && SendMessageW(pw, CB_GETCOUNT, 0, 0) == 3 && sel == 2 && wcscmp(item, L"When the game closes") == 0,
              "Convert recordings drop-down shows post_when = game_exit from the file ('When the game closes')");
        SendMessageW(pw, CB_SETCURSEL, 0, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_POSTWHEN, CBN_SELCHANGE), (LPARAM)pw);
    }
    // "Crop each eye" drop-down (eye_crop): loaded as item 0 "16:9 (...)"; item 1 = 4:3
    {
        const HWND ec = GetDlgItem(d, settings::ID_EYECROP);
        wchar_t item[128] = L"";
        const int sel = (int)SendMessageW(ec, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(ec, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    Crop each eye drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(ec, CB_GETCOUNT, 0, 0), sel, N(item).c_str());
        Check(ec != nullptr && SendMessageW(ec, CB_GETCOUNT, 0, 0) == 3 && sel == 0 && wcsncmp(item, L"16:9 (wide", 10) == 0,
              "Crop each eye drop-down shows eye_crop = 16:9 from the file");
        SendMessageW(ec, CB_SETCURSEL, 1, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_EYECROP, CBN_SELCHANGE), (LPARAM)ec);
    }
    if (!leaveSlider) SendMessageW(GetDlgItem(d, settings::ID_SMOOTH), TBM_SETPOS, TRUE, 7);   // 0.35 ("noslider": untouched keeps the file text)
    Check(SendMessageW(GetDlgItem(d, settings::ID_AUTOSTART), BM_GETCHECK, 0, 0) == BST_UNCHECKED, "Start with Windows: unchecked by default");
    SendMessageW(GetDlgItem(d, settings::ID_AUTOSTART), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(d, settings::ID_SAVE), BM_CLICK, 0, 0);
    Check(WaitDialog(pid, false) == nullptr, "good values: Save closed the window");
    std::wstring stored;
    Check(install::AutostartEnabled(&stored), "Start with Windows ticked + Save: the (test) Run value exists");
    Check(stored.size() > 2 && stored.front() == L'"' && stored.find(L"EyeCaptureTray.exe") != std::wstring::npos, "Run value is a quoted EyeCaptureTray.exe path");
    printf("    Run value: %s\n", N(stored).c_str());
    { std::wstring e; install::SetAutostart(false, L"", e); }
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test\\Run");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test");   // only succeeds when empty
    install::SetAutostartTestKey(L"");
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
