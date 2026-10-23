// settings_test -- tests for the tray's app window (tray/settings.cpp). No GPU, no game.
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
//   settings_test svr <tray-pid> <list|enable|enableall|disableall|toggle|click|disable|reenable|repair|rescan|unregister>
//                     [row=<game name>] [answers=Yes,No,OK] [via=balloon]
//       drives the REAL "SteamVR games" window of a scratch tray (tray/steamvr_ui.h; tests/run_steamvr_ui.ps1): posts
//       WM_APP+9 (via=balloon: a click on the tray's last balloon instead, WM_APP+1 with NIN_BALLOONUSERCLICK -- the window must
//       open from it), waits until the Steam scan is done, selects the row of that game, does the action (enable = "Add a game
//       by exe...", toggle = the check box through ID_SVR_TOGGLE, click = a real mouse click into the row's check box), then
//       answers every message box the window shows with the next answer (printed "BOX: <text>" / "ANSWER: <button>"; no
//       answers left: OK, else No), and prints the rows ("ROW: game | state | folder"), the status line, the details text
//       and the button states before closing it. unregister: posts WM_APP+11 (the tray menu's "Unregister from VR games...";
//       its question boxes are answered by the tray's EYECAP_TRAY_TEST_ASK file) and waits 3 s, no window.
// Exit code 0 = all checks passed.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>   // NIN_BALLOONUSERCLICK
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "config.h"
#include "install.h"
#include "postprocess.h"
#include "settings.h"
#include "steamvr_ui.h"
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
        ParseHotkey(kDefaultHotkey, bad.hotkey);
        bad.hotkey.text = L"Ctrl+Banana";
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(path, bad, r);
        printf("  hotkey = Ctrl+Banana -> ok=%d, problems:\n", ok);
        for (const auto& p : r.problems) printf("      %s\n", N(p).c_str());
        Check(!ok && !r.problems.empty() && ReadAll(path) == original, "hotkey Ctrl+Banana is refused and the file is unchanged");
    }
    // the built-in hotkey default (= the shipped config.ini, Restore Defaults): ScrollLock
    {
        const std::wstring p0 = path + L".nohotkey-test.ini";
        { std::ofstream f(p0, std::ios::binary); f << "[capture]\r\ncodec = hevc\r\n"; }
        Config d;
        std::vector<std::wstring> pr;
        ParseConfigFile(p0, d, pr);
        Check(pr.empty() && d.hotkey.text == L"ScrollLock" && d.hotkey.bindings.size() == 1, "no hotkey line -> the default ScrollLock");
        DeleteFileW(p0.c_str());
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
        ParseHotkey(kDefaultHotkey, n.hotkey);
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
        ParseHotkey(kDefaultHotkey, n.hotkey);
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
        { std::ofstream f(p4, std::ios::binary); f << "[post_processing]\r\npost_yt8k = 1\r\nlayout_3d = tb_half\r\n"; }
        const Layout3d all[] = {Layout3d::SbsHalf, Layout3d::SbsFull, Layout3d::TbFull, Layout3d::TbHalf};
        const char* names[] = {"sbs_half", "sbs_full", "tb_full", "tb_half"};
        for (int i = 0; i < 4; ++i) {
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
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
        { std::ofstream f(p4, std::ios::binary); f << "[post_processing]\r\nlayout_3d = Sbs_Full\r\n"; }
        Config ci;
        std::vector<std::wstring> pr;
        ParseConfigFile(p4, ci, pr);
        Check(pr.empty() && ci.layout3d == Layout3d::SbsFull, "layout_3d is case-insensitive (Sbs_Full)");
        { std::ofstream f(p4, std::ios::binary); f << "[post_processing]\r\nlayout_3d = sideways\r\n"; }
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
        const PostWhen all[] = {PostWhen::Now, PostWhen::SessionEnd, PostWhen::GameExit, PostWhen::Manual};
        const char* names[] = {"now", "session_end", "game_exit", "manual"};
        for (int i = 0; i < 4; ++i) {
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
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
        ParseHotkey(kDefaultHotkey, dflt.hotkey);
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
        const EyeCrop all[] = {EyeCrop::Std43, EyeCrop::None, EyeCrop::Pad43, EyeCrop::Wide169};
        const char* names[] = {"4:3", "none", "4:3pad", "16:9"};
        for (int i = 0; i < 4; ++i) {
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
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
        { std::ofstream f(p6, std::ios::binary); f << "[post_processing]\r\neye_crop = 4:3PAD\r\n"; }
        Config cp;
        std::vector<std::wstring> prp;
        ParseConfigFile(p6, cp, prp);
        Check(prp.empty() && cp.eyeCrop == EyeCrop::Pad43, "eye_crop is case-insensitive (4:3PAD)");
        Check(pp::CropFromName("4:3pad") == EyeCrop::Pad43 && pp::CropFromName("4:3") == EyeCrop::Std43 && pp::CropFromName("none") == EyeCrop::None &&
                  pp::CropFromName("x") == EyeCrop::Wide169 && wcscmp(pp::CropName(EyeCrop::Pad43), L"4:3pad") == 0,
              "sidecar / queue.json crop names: 4:3pad <-> EyeCrop::Pad43");
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
        ParseHotkey(kDefaultHotkey, n.hotkey);
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
    // 10. max_width ("Resolution"): default 0; the drop-down values and a custom one are saved in place in [capture] and
    // read back; a value outside 0 / 256..16384 is reported and keeps 0
    {
        Config def;
        Check(def.maxWidth == 0, "max_width defaults to 0 (native)");
        const std::wstring p8 = path + L".maxwidth-test.ini";
        { std::ofstream f(p8, std::ios::binary); f << "[capture]\r\nmax_fps = 0\r\nmax_width = 0\r\n\r\n[debug]\r\nlog_level = info\r\n"; }
        const unsigned all[] = {3840, 2560, 1920, 3000, 0};
        for (unsigned w : all) {
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
            n.maxWidth = w;
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p8, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(p8, back, pr);
            const std::string text = ReadAll(p8);
            const std::string want = "max_width = " + std::to_string(w) + "\r\n";
            Check(ok && pr.empty() && back.maxWidth == w && text.find(want) != std::string::npos &&
                      text.find("max_width", text.find("max_width") + 1) == std::string::npos &&
                      text.find("max_width") < text.find("[debug]"),
                  ("max_width = " + std::to_string(w) + " is saved in place in [capture] and read back").c_str());
        }
        { std::ofstream f(p8, std::ios::binary); f << "[capture]\r\nmax_width = 100\r\n"; }
        Config bad;
        std::vector<std::wstring> pr2;
        ParseConfigFile(p8, bad, pr2);
        Check(pr2.size() == 1 && bad.maxWidth == 0, "max_width = 100: reported as a bad value, 0 kept");
        // an odd value is read rounded down to even (3841 -> 3840); saving another key must keep the exact file text 3841
        { std::ofstream f(p8, std::ios::binary); f << "[capture]\r\nmax_width = 3841\r\nmax_fps = 0\r\n"; }
        {
            Config odd;
            std::vector<std::wstring> pr;
            ParseConfigFile(p8, odd, pr);
            odd.maxFps = 30;
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p8, odd, r);
            const std::string text = ReadAll(p8);
            Check(odd.maxWidth == 3840 && ok && r.changed.size() == 1 && text.find("max_width = 3841\r\n") != std::string::npos &&
                      text.find("max_fps = 30\r\n") != std::string::npos,
                  "max_width = 3841 (read as 3840) is left alone when only max_fps is saved");
        }
        // a pre-existing bad line the window does not edit (dome_width) moves down when Save inserts a missing key above it:
        // its "line N:" changes, it is still the same problem -> Save is not refused
        { std::ofstream f(p8, std::ios::binary); f << "[capture]\r\nmax_fps = 0\r\n\r\n[dome]\r\ndome_width = abc\r\n"; }
        {
            Config c;
            std::vector<std::wstring> pr;
            ParseConfigFile(p8, c, pr);
            c.maxWidth = 3840;
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p8, c, r);
            const std::string text = ReadAll(p8);
            for (const auto& p : r.problems) printf("    problem: %s\n", N(p).c_str());
            Check(pr.size() == 1 && ok && r.problems.empty() && text.find("max_width = 3840\r\n") != std::string::npos &&
                      text.find("max_width") < text.find("[dome]") && text.find("dome_width = abc\r\n") != std::string::npos,
                  "max_width inserted above an old bad line (dome_width = abc, line 5 -> 6): Save is not refused, the bad line is kept");
            Config again;
            std::vector<std::wstring> pr3;
            ParseConfigFile(p8, again, pr3);
            Check(pr3.size() == 1 && pr3[0].compare(0, 8, L"line 6: ") == 0, "the old bad line is now reported as line 6 (same problem)");
        }
        DeleteFileW(p8.c_str());
    }
    // 11. yt8k_bitrate_mbps ("8K bitrate"): default 150; 100 / 200 / a custom 180 saved in place and read back; a missing key +
    // the default leaves the file alone; 401 and 10 are reported and keep 150
    {
        Config def;
        Check(def.yt8kBitrateMbps == 150, "yt8k_bitrate_mbps defaults to 150");
        const std::wstring p9 = path + L".yt8k-test.ini";
        { std::ofstream f(p9, std::ios::binary); f << "[post_processing]\r\npost_yt8k = 1\r\n"; }
        Config n0;
        ParseHotkey(kDefaultHotkey, n0.hotkey);
        n0.postYt8k = true;
        settings::SaveResult r0;
        Check(settings::SaveSettingsToFile(p9, n0, r0) && ReadAll(p9) == "[post_processing]\r\npost_yt8k = 1\r\n",
              "missing yt8k_bitrate_mbps + default 150: the file is left alone");
        for (unsigned v : {100u, 200u, 180u, 150u}) {
            Config n = n0;
            n.yt8kBitrateMbps = v;
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(p9, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(p9, back, pr);
            const std::string text = ReadAll(p9);
            Check(ok && pr.empty() && back.yt8kBitrateMbps == v && text.find("yt8k_bitrate_mbps = " + std::to_string(v) + "\r\n") != std::string::npos &&
                      text.find("yt8k_bitrate_mbps", text.find("yt8k_bitrate_mbps") + 1) == std::string::npos,
                  ("yt8k_bitrate_mbps = " + std::to_string(v) + " is saved in place and read back").c_str());
        }
        for (const char* badv : {"401", "10"}) {
            { std::ofstream f(p9, std::ios::binary); f << "[post_processing]\r\nyt8k_bitrate_mbps = " << badv << "\r\n"; }
            Config bad;
            std::vector<std::wstring> pr2;
            ParseConfigFile(p9, bad, pr2);
            Check(pr2.size() == 1 && bad.yt8kBitrateMbps == 150, (std::string("yt8k_bitrate_mbps = ") + badv + ": reported, 150 kept").c_str());
        }
        DeleteFileW(p9.c_str());
    }
    // 12. yt8k_codec ("8K codec"): default hevc; a missing key + hevc leaves the file alone; av1 is added to [post_processing],
    // then hevc / av1 are saved in place and read back; case-insensitive; vp9 is reported and keeps hevc
    {
        Config def;
        Check(def.yt8kCodec == VideoCodec::Hevc, "yt8k_codec defaults to hevc");
        const std::wstring pa = path + L".yt8kcodec-test.ini";
        { std::ofstream f(pa, std::ios::binary); f << "[post_processing]\r\npost_yt8k = 1\r\n"; }
        Config n0;
        ParseHotkey(kDefaultHotkey, n0.hotkey);
        n0.postYt8k = true;
        settings::SaveResult r0;
        Check(settings::SaveSettingsToFile(pa, n0, r0) && ReadAll(pa) == "[post_processing]\r\npost_yt8k = 1\r\n",
              "missing yt8k_codec + default hevc: the file is left alone");
        for (VideoCodec v : {VideoCodec::Av1, VideoCodec::Hevc, VideoCodec::Av1}) {
            Config n = n0;
            n.yt8kCodec = v;
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(pa, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(pa, back, pr);
            const std::string text = ReadAll(pa);
            const std::string want = std::string("yt8k_codec = ") + (v == VideoCodec::Av1 ? "av1" : "hevc") + "\r\n";
            Check(ok && pr.empty() && back.yt8kCodec == v && text.find(want) != std::string::npos &&
                      text.find("yt8k_codec", text.find("yt8k_codec") + 1) == std::string::npos,
                  ("yt8k_codec = " + std::string(v == VideoCodec::Av1 ? "av1" : "hevc") + " is saved (once) and read back").c_str());
        }
        for (const char* val : {"AV1", "H265"}) {
            { std::ofstream f(pa, std::ios::binary); f << "[post_processing]\r\nyt8k_codec = " << val << "\r\n"; }
            Config c2;
            std::vector<std::wstring> pr2;
            ParseConfigFile(pa, c2, pr2);
            Check(pr2.empty() && c2.yt8kCodec == (val[0] == 'A' ? VideoCodec::Av1 : VideoCodec::Hevc),
                  (std::string("yt8k_codec = ") + val + " is read case-insensitively").c_str());
        }
        { std::ofstream f(pa, std::ios::binary); f << "[post_processing]\r\nyt8k_codec = vp9\r\n"; }
        Config bad;
        std::vector<std::wstring> pr3;
        ParseConfigFile(pa, bad, pr3);
        Check(pr3.size() == 1 && bad.yt8kCodec == VideoCodec::Hevc, "yt8k_codec = vp9: reported, hevc kept");
        DeleteFileW(pa.c_str());
    }
    // 13. codec = av1 (recording): read, saved in place (hevc -> av1 -> h264 -> av1), a bad value keeps hevc
    {
        const std::wstring pc = path + L".codec-test.ini";
        { std::ofstream f(pc, std::ios::binary); f << "[capture]\r\ncodec = hevc\r\n"; }
        for (VideoCodec v : {VideoCodec::Av1, VideoCodec::H264, VideoCodec::Av1}) {
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
            n.codec = v;
            settings::SaveResult r;
            const bool ok = settings::SaveSettingsToFile(pc, n, r);
            Config back;
            std::vector<std::wstring> pr;
            ParseConfigFile(pc, back, pr);
            const std::string text = ReadAll(pc);
            Check(ok && pr.empty() && back.codec == v && text == std::string("[capture]\r\ncodec = ") + VideoCodecKey(v) + "\r\n",
                  (std::string("codec = ") + VideoCodecKey(v) + " is saved in place and read back").c_str());
        }
        { std::ofstream f(pc, std::ios::binary); f << "[capture]\r\ncodec = vp9\r\n"; }
        Config bad;
        std::vector<std::wstring> pr2;
        ParseConfigFile(pc, bad, pr2);
        Check(pr2.size() == 1 && bad.codec == VideoCodec::Hevc, "codec = vp9: reported, hevc kept");
        // 14. record_for_youtube: default 0; missing + 0 leaves the file alone; 1 is appended to [capture]; 0 again in place
        {
            Config def;
            Check(!def.recordForYoutube, "record_for_youtube defaults to 0");
            const std::wstring pr4 = path + L".recordyt-test.ini";
            { std::ofstream f(pr4, std::ios::binary); f << "[capture]\r\nswap_eyes = 0\r\n\r\n[debug]\r\nlog_level = info\r\n"; }
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
            settings::SaveResult r1;
            Check(settings::SaveSettingsToFile(pr4, n, r1) && ReadAll(pr4) == "[capture]\r\nswap_eyes = 0\r\n\r\n[debug]\r\nlog_level = info\r\n",
                  "missing record_for_youtube + default 0: the file is left alone");
            n.recordForYoutube = true;
            settings::SaveResult r2;
            const bool ok2 = settings::SaveSettingsToFile(pr4, n, r2);
            Config back;
            std::vector<std::wstring> prb;
            ParseConfigFile(pr4, back, prb);
            Check(ok2 && r2.changed.size() == 1 && prb.empty() && back.recordForYoutube &&
                      ReadAll(pr4) == "[capture]\r\nswap_eyes = 0\r\nrecord_for_youtube = 1\r\n\r\n[debug]\r\nlog_level = info\r\n",
                  "record_for_youtube = 1: appended to the end of [capture] and read back as on");
            n.recordForYoutube = false;
            settings::SaveResult r3;
            const bool ok3 = settings::SaveSettingsToFile(pr4, n, r3);
            ParseConfigFile(pr4, back, prb);
            Check(ok3 && r3.changed.size() == 1 && r3.changed[0] == L"record_for_youtube 1->0" && !back.recordForYoutube &&
                      ReadAll(pr4).find("record_for_youtube = 0\r\n") != std::string::npos,
                  "record_for_youtube back to 0: changed in place ('record_for_youtube 1->0')");
            DeleteFileW(pr4.c_str());
        }
        // 14b. tag_3d: default 1; missing + 1 leaves the file alone; 0 is appended to [capture]; 1 again in place
        {
            Config def;
            Check(def.tag3d, "tag_3d defaults to 1");
            const std::wstring pt = path + L".tag3d-test.ini";
            { std::ofstream f(pt, std::ios::binary); f << "[capture]\r\nswap_eyes = 0\r\n"; }
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
            settings::SaveResult r1;
            Check(settings::SaveSettingsToFile(pt, n, r1) && ReadAll(pt) == "[capture]\r\nswap_eyes = 0\r\n",
                  "missing tag_3d + default 1: the file is left alone");
            n.tag3d = false;
            settings::SaveResult r2;
            const bool ok2 = settings::SaveSettingsToFile(pt, n, r2);
            Config back;
            std::vector<std::wstring> prb;
            ParseConfigFile(pt, back, prb);
            Check(ok2 && r2.changed.size() == 1 && prb.empty() && !back.tag3d &&
                      ReadAll(pt) == "[capture]\r\nswap_eyes = 0\r\ntag_3d = 0\r\n",
                  "tag_3d = 0: appended to [capture] and read back as off");
            n.tag3d = true;
            settings::SaveResult r3;
            const bool ok3 = settings::SaveSettingsToFile(pt, n, r3);
            ParseConfigFile(pt, back, prb);
            Check(ok3 && r3.changed.size() == 1 && r3.changed[0] == L"tag_3d 0->1" && back.tag3d,
                  "tag_3d back to 1: changed in place ('tag_3d 0->1')");
            DeleteFileW(pt.c_str());
        }
        // 14c. dome_shared_view (the "Both eyes only (shared view)" check): default 1; 0 is appended to [dome]; 1 again in place
        {
            Config def;
            Check(def.dome.sharedView, "dome_shared_view defaults to 1");
            const std::wstring pd = path + L".sharedview-test.ini";
            { std::ofstream f(pd, std::ios::binary); f << "[dome]\r\nfill = black\r\n"; }
            Config n;
            ParseHotkey(kDefaultHotkey, n.hotkey);
            n.dome.fill = DomeFill::Black;
            n.dome.sharedView = false;
            settings::SaveResult r1;
            const bool ok1 = settings::SaveSettingsToFile(pd, n, r1);
            Config back;
            std::vector<std::wstring> prb;
            ParseConfigFile(pd, back, prb);
            Check(ok1 && r1.changed.size() == 1 && prb.empty() && !back.dome.sharedView &&
                      ReadAll(pd) == "[dome]\r\nfill = black\r\ndome_shared_view = 0\r\n",
                  "dome_shared_view = 0: appended to [dome] and read back as off");
            n.dome.sharedView = true;
            settings::SaveResult r2;
            const bool ok2 = settings::SaveSettingsToFile(pd, n, r2);
            ParseConfigFile(pd, back, prb);
            Check(ok2 && r2.changed.size() == 1 && r2.changed[0] == L"dome_shared_view 0->1" && back.dome.sharedView,
                  "dome_shared_view back to 1: changed in place ('dome_shared_view 0->1')");
            DeleteFileW(pd.c_str());
        }
        Check(VideoCodecDefaultMbps(VideoCodec::Av1) == 80 && VideoCodecDefaultMbps(VideoCodec::Hevc) == 120 &&
                  VideoCodecDefaultMbps(VideoCodec::H264) == 80 && VideoCodecMaxSide(VideoCodec::Av1) == 8192 &&
                  VideoCodecMaxSide(VideoCodec::H264) == 4096,
              "codec defaults: AV1 80 / HEVC 120 / H.264 80 Mbit/s; frame limit AV1 8192, H.264 4096");
        DeleteFileW(pc.c_str());
    }
    // 14. issue #1 keys: beeps, rate_control, qp, quality_vs_speed, constant_frame_rate. Defaults; missing keys + defaults leave
    // the file alone; each non-default value is appended to [capture] and read back; bad values are reported and keep the default.
    {
        Config def;
        Check(def.beeps && def.rateControl == RateControl::Vbr && def.qp == 24 && def.qualityVsSpeed == -1 && !def.constantFrameRate,
              "defaults: beeps 1, rate_control vbr, qp 24, quality_vs_speed auto (-1), constant_frame_rate 0");
        const std::wstring pn = path + L".issue1-test.ini";
        const std::string base = "[capture]\r\nswap_eyes = 0\r\n\r\n[debug]\r\nlog_level = info\r\n";
        { std::ofstream f(pn, std::ios::binary); f << base; }
        Config n;
        ParseHotkey(kDefaultHotkey, n.hotkey);
        settings::SaveResult r0;
        Check(settings::SaveSettingsToFile(pn, n, r0) && r0.changed.empty() && ReadAll(pn) == base,
              "missing beeps / rate_control / qp / quality_vs_speed / constant_frame_rate + defaults: the file is left alone");
        n.beeps = false; n.rateControl = RateControl::Qp; n.qp = 18; n.qualityVsSpeed = 75; n.constantFrameRate = true;
        settings::SaveResult r1;
        const bool ok1 = settings::SaveSettingsToFile(pn, n, r1);
        Config back;
        std::vector<std::wstring> prb;
        ParseConfigFile(pn, back, prb);
        const std::string t1 = ReadAll(pn);
        Check(ok1 && r1.changed.size() == 5 && prb.empty() && !back.beeps && back.rateControl == RateControl::Qp && back.qp == 18 &&
                  back.qualityVsSpeed == 75 && back.constantFrameRate && t1.find("rate_control = qp\r\n") != std::string::npos &&
                  t1.find("quality_vs_speed = 75\r\n") != std::string::npos && t1.find("[debug]") > t1.find("constant_frame_rate = 1"),
              "beeps 0, rate_control qp, qp 18, quality_vs_speed 75, constant_frame_rate 1: appended to [capture] and read back");
        n.qualityVsSpeed = -1; n.rateControl = RateControl::Cbr;
        settings::SaveResult r2;
        const bool ok2 = settings::SaveSettingsToFile(pn, n, r2);
        ParseConfigFile(pn, back, prb);
        const std::string t2 = ReadAll(pn);
        Check(ok2 && r2.changed.size() == 2 && back.qualityVsSpeed == -1 && back.rateControl == RateControl::Cbr &&
                  t2.find("quality_vs_speed = auto\r\n") != std::string::npos && t2.find("rate_control = cbr\r\n") != std::string::npos,
              "quality_vs_speed back to auto and rate_control cbr: changed in place");
        // Norm: "AUTO" / "Auto" and "cqp" are the same values (no change reported)
        { std::ofstream f(pn, std::ios::binary); f << "[capture]\r\nquality_vs_speed = AUTO\r\nrate_control = cqp\r\nqp = 024\r\n"; }
        Config same;
        std::vector<std::wstring> prs;
        ParseConfigFile(pn, same, prs);
        settings::SaveResult r3;
        Check(prs.empty() && same.qualityVsSpeed == -1 && same.rateControl == RateControl::Qp && same.qp == 24 &&
                  settings::SaveSettingsToFile(pn, same, r3) && r3.changed.empty(),
              "quality_vs_speed = AUTO, rate_control = cqp, qp = 024 parse and count as unchanged on Save");
        // bad values: qp 52, quality_vs_speed 101 / fast, rate_control abr, beeps maybe, constant_frame_rate 2
        { std::ofstream f(pn, std::ios::binary);
          f << "[capture]\r\nqp = 52\r\nquality_vs_speed = 101\r\nrate_control = abr\r\nbeeps = maybe\r\nconstant_frame_rate = 2\r\n"
               "quality_vs_speed = fast\r\n"; }
        Config bad;
        std::vector<std::wstring> prx;
        ParseConfigFile(pn, bad, prx);
        for (const auto& p : prx) printf("    problem: %s\n", N(p).c_str());
        Check(prx.size() == 6 && bad.qp == 24 && bad.qualityVsSpeed == -1 && bad.rateControl == RateControl::Vbr && bad.beeps &&
                  !bad.constantFrameRate,
              "qp 52, quality_vs_speed 101 / fast, rate_control abr, beeps maybe, constant_frame_rate 2: 6 bad values, defaults kept");
        DeleteFileW(pn.c_str());
    }
    // the Settings window writes steamvr_mode only when its "SteamVR games" drop-down was changed (the SteamVR games window sets
    // it too) and Save re-reads the file, so a steamvr_mode written while the window was open survives its Save
    {
        const std::wstring pn = path + L".steamvr.ini";
        { std::ofstream f(pn, std::ios::binary); f << "[capture]\r\nbitrate_mbps = 0\r\n\r\n[steamvr]\r\nsteamvr_mode = per_game\r\n"; }
        Config c;
        std::vector<std::wstring> pr;
        ParseConfigFile(pn, c, pr);   // what an open window holds
        { std::ofstream f(pn, std::ios::binary); f << "[capture]\r\nbitrate_mbps = 0\r\n\r\n[steamvr]\r\nsteamvr_mode = quick\r\n"; }   // written meanwhile
        c.bitrateMbps = 150;
        settings::SaveResult r;
        const bool ok = settings::SaveSettingsToFile(pn, c, r);
        const std::string t = ReadAll(pn);
        Check(ok && t.find("steamvr_mode = quick\r\n") != std::string::npos && t.find("bitrate_mbps = 150\r\n") != std::string::npos &&
                  t.find("per_game") == std::string::npos,
              "Save keeps a steamvr_mode written while the window was open (only its own keys change)");
        DeleteFileW(pn.c_str());
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
    // visible only: the window exists (with its title) while its controls are still being created and loaded; it is shown last
    if (pid == c->pid && wcscmp(t, settings::kWindowTitle) == 0 && IsWindowVisible(h)) { c->found = h; return FALSE; }
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
// The window has tabs: show the tab that holds `c` (kMsgShowTabOf), then whether `c` is visible.
bool OnTab(HWND c) {
    if (!c) return false;
    SendMessageW(GetParent(c), settings::kMsgShowTabOf, (WPARAM)GetDlgCtrlID(c), 0);
    return IsWindowVisible(c) != FALSE;
}
void SetEdit(HWND d, int id, const wchar_t* t) { SendMessageW(GetDlgItem(d, id), WM_SETTEXT, 0, (LPARAM)t); }
std::wstring GetEdit(HWND d, int id) {
    wchar_t b[512] = L"";
    SendMessageW(GetDlgItem(d, id), WM_GETTEXT, 512, (LPARAM)b);
    return b;
}
// A child of the dialog by its exact text (row labels have no control id).
HWND ChildByText(HWND d, const wchar_t* text) {
    struct Ctx { const wchar_t* text; HWND found; } ctx{text, nullptr};
    EnumChildWindows(d, [](HWND c, LPARAM lp) -> BOOL {
        Ctx* x = (Ctx*)lp;
        wchar_t b[128] = L"";
        GetWindowTextW(c, b, 128);
        if (wcscmp(b, x->text) == 0) { x->found = c; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}
HWND FindTop(DWORD pid, const wchar_t* cls, const wchar_t* title);   // defined with the svr test below
int TestDrive(DWORD pid, bool leaveSlider) {
    printf("drive the app window of tray pid %lu\n", pid);
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
    Check(d != nullptr, "app window opened");
    if (!d) return 1;
    printf("  window title '%s', hotkey field '%s', bitrate '%s'\n", N(settings::kWindowTitle).c_str(), N(GetEdit(d, settings::ID_HOTKEY)).c_str(),
           N(GetEdit(d, settings::ID_BITRATE)).c_str());
    // "Capture...": F7 without modifiers
    SendMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), BM_CLICK, 0, 0);
    PostMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), WM_KEYDOWN, VK_F7, 0);
    Sleep(300);
    Check(GetEdit(d, settings::ID_HOTKEY) == L"F7", "'Capture...' captured F7 into the hotkey field");
    // "Add...": the mouse side button X1 is appended to the list
    SendMessageW(GetDlgItem(d, settings::ID_HOTKEY_ADD), BM_CLICK, 0, 0);
    PostMessageW(d, WM_XBUTTONDOWN, (WPARAM)XBUTTON1 << 16, 0);
    Sleep(300);
    printf("    after Add... + mouse X1: '%s'\n", N(GetEdit(d, settings::ID_HOTKEY)).c_str());
    Check(GetEdit(d, settings::ID_HOTKEY) == L"F7, MouseX1", "'Add...' appended MouseX1: 'F7, MouseX1'");
    wchar_t btn[128] = L"";
    GetWindowTextW(GetDlgItem(d, settings::ID_HOTKEY_ADD), btn, 128);
    Check(wcscmp(btn, L"Add...") == 0, "the Add button is disarmed again after the capture");
    // "Capture..." again replaces the whole list
    SendMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), BM_CLICK, 0, 0);
    PostMessageW(GetDlgItem(d, settings::ID_HOTKEY_PICK), WM_KEYDOWN, VK_F7, 0);
    Sleep(300);
    Check(GetEdit(d, settings::ID_HOTKEY) == L"F7", "'Capture...' + F7 replaced the list: 'F7'");
    // bad value: Save must refuse and keep the window open
    SetEdit(d, settings::ID_FOVSCALE, L"7");
    SendMessageW(GetDlgItem(d, settings::ID_SAVE), BM_CLICK, 0, 0);
    Sleep(300);
    Check(FindDialog(pid) != nullptr, "bad output_fov_scale: Save refused, window stays open (message logged by the tray)");
    {
        const HWND lp = GetDlgItem(d, settings::ID_LAYOUTPREVIEW);
        RECT lr{};
        if (lp) GetWindowRect(lp, &lr);
        Check(lp != nullptr && OnTab(lp) && lr.right - lr.left >= 200 && lr.bottom - lr.top >= 130,
              "3D layout preview control exists and is visible");
        const WPARAM lay0 = (WPARAM)SendMessageW(GetDlgItem(d, settings::ID_LAYOUT3D), CB_GETCURSEL, 0, 0);
        SendMessageW(GetDlgItem(d, settings::ID_LAYOUT3D), CB_SETCURSEL, 1, 0);   // sbs_half: repaint must not crash
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_LAYOUT3D, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_LAYOUT3D));
        UpdateWindow(lp);
        SendMessageW(GetDlgItem(d, settings::ID_LAYOUT3D), CB_SETCURSEL, lay0, 0);   // back: the saved diff below must not change
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_LAYOUT3D, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_LAYOUT3D));
        Check(IsWindow(d), "changing the 3D layout repaints the preview and leaves the window sane");
    }
    // indicator preview: the owner-drawn control exists; editing indicator_x / toggling the check only repaints it (the saved diff
    // below must stay exactly the expected keys: indicator_x goes back to its original text)
    {
        const HWND pv = GetDlgItem(d, settings::ID_INDPREVIEW);
        RECT pr{};
        if (pv) GetWindowRect(pv, &pr);
        Check(pv != nullptr && OnTab(pv) && pr.right - pr.left >= 100 && pr.right - pr.left == pr.bottom - pr.top,
              "indicator preview control exists, visible and square");
        const std::wstring x0 = GetEdit(d, settings::ID_INDX), z0 = GetEdit(d, settings::ID_INDZ);
        SetEdit(d, settings::ID_INDX, L"0.5");
        SetEdit(d, settings::ID_INDX, L"not a number");   // invalid text keeps the last good value in the preview, nothing crashes
        SetEdit(d, settings::ID_INDZ, L"0.3");            // z >= 0: "behind you"
        UpdateWindow(pv);
        SendMessageW(GetDlgItem(d, settings::ID_INDICATOR), BM_CLICK, 0, 0);   // off
        SendMessageW(GetDlgItem(d, settings::ID_INDICATOR), BM_CLICK, 0, 0);   // on again
        UpdateWindow(pv);
        // mouse on the preview: click / drag place the dot (X / Y; Z kept), the wheel over it changes Z (angles kept). The circle
        // is +-50 degrees: half the radius = 25 degrees = tan(25) x 0.8 m = 0.37 m; past the edge = 50 degrees = 0.95 m.
        {
            const std::wstring y0 = GetEdit(d, settings::ID_INDY);
            RECT cr{};
            GetClientRect(pv, &cr);
            const int pcx = cr.right / 2, pcy = cr.bottom / 2, R = (cr.right < cr.bottom ? cr.right : cr.bottom) / 2 - 4;
            auto click = [&](int x, int y) {
                SendMessageW(pv, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                SendMessageW(pv, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            };
            auto at = [&](const wchar_t* x, const wchar_t* y, const wchar_t* z) {
                return GetEdit(d, settings::ID_INDX) == x && GetEdit(d, settings::ID_INDY) == y && GetEdit(d, settings::ID_INDZ) == z;
            };
            SetEdit(d, settings::ID_INDZ, L"0.3");   // behind you: a click first brings the dot to 0.8 m
            click(pcx, pcy);
            Check(at(L"0.00", L"0.00", L"-0.80"), "preview click in the centre (dot was behind you): X 0.00, Y 0.00, Z -0.80");
            click(pcx + R / 2, pcy);
            Check(at(L"0.37", L"0.00", L"-0.80"), "preview click half way right: X 0.37 (25 deg at 0.8 m), Y 0.00, Z kept");
            click(pcx, pcy - R / 2);
            Check(at(L"0.00", L"0.37", L"-0.80"), "preview click half way up: Y +0.37 (up is +Y)");
            SendMessageW(pv, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pcx, pcy));
            SendMessageW(pv, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(pcx, pcy + R / 2));
            SendMessageW(pv, WM_LBUTTONUP, 0, MAKELPARAM(pcx, pcy + R / 2));
            Check(at(L"0.00", L"-0.37", L"-0.80"), "preview drag from the centre down: Y -0.37");
            click(pcx + 3 * R, pcy);
            Check(at(L"0.95", L"0.00", L"-0.80"), "preview click past the edge: held on the circle (50 deg = X 0.95)");
            click(pcx + R / 2, pcy);
            POINT sp{pcx, pcy};
            ClientToScreen(pv, &sp);
            SendMessageW(d, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(sp.x, sp.y));
            Check(at(L"0.35", L"0.00", L"-0.75"), "mouse wheel up over the preview: Z -0.75 (5 cm nearer), X 0.35 (same angle)");
            SetEdit(d, settings::ID_INDY, y0.c_str());
        }
        SetEdit(d, settings::ID_INDX, x0.c_str());
        SetEdit(d, settings::ID_INDZ, z0.c_str());
        Check(IsWindow(d) && GetEdit(d, settings::ID_INDX) == x0, "indicator edits / check toggle with the preview open leave the window sane");
    }
    // good values
    SetEdit(d, settings::ID_FOVSCALE, L"0.85");
    // "Bitrate" drop-down (bitrate_mbps): loaded 0 = "Default (120 Mbit/s)", hint "about 54 GB per hour"; pick 250 -> 113 GB/h
    {
        const HWND br = GetDlgItem(d, settings::ID_BITRATE);
        wchar_t item[64] = L"";
        const int sel = (int)SendMessageW(br, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(br, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        const std::wstring hint0 = GetEdit(d, settings::ID_BITRATEHINT);
        printf("    Bitrate drop-down: %d items, selected %d '%s', hint '%s'\n", (int)SendMessageW(br, CB_GETCOUNT, 0, 0), sel, N(item).c_str(),
               N(hint0).c_str());
        Check(br && SendMessageW(br, CB_GETCOUNT, 0, 0) == 9 && sel == 0 && wcscmp(item, L"Default (120 Mbit/s)") == 0 &&
                  hint0 == L"about 54 GB per hour",
              "Bitrate drop-down shows bitrate_mbps = 0 as 'Default (120 Mbit/s)', hint 'about 54 GB per hour'");
        // "Codec" drop-down: 3 items, loaded hevc; pick AV1 (item 2): the Default hint follows the AV1 default (80 Mbit/s)
        const HWND cc = GetDlgItem(d, settings::ID_CODEC);
        wchar_t citem[64] = L"";
        SendMessageW(cc, CB_GETLBTEXT, 2, (LPARAM)citem);
        Check(cc && SendMessageW(cc, CB_GETCOUNT, 0, 0) == 3 && SendMessageW(cc, CB_GETCURSEL, 0, 0) == 0 &&
                  wcscmp(citem, L"AV1 (hardware: RTX 40 / 50)") == 0,
              "Codec drop-down has 3 items (HEVC loaded, item 2 'AV1 (hardware: RTX 40 / 50)')");
        SendMessageW(cc, CB_SETCURSEL, 2, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_CODEC, CBN_SELCHANGE), (LPARAM)cc);
        printf("    Codec AV1 + Default bitrate: hint '%s'\n", N(GetEdit(d, settings::ID_BITRATEHINT)).c_str());
        Check(GetEdit(d, settings::ID_BITRATEHINT) == L"about 36 GB per hour (AV1: 80 Mbit/s)",
              "Codec AV1 + Default bitrate: hint 'about 36 GB per hour (AV1: 80 Mbit/s)'");
        {
            wchar_t i0[64] = L"";
            SendMessageW(br, CB_GETLBTEXT, 0, (LPARAM)i0);
            Check(wcscmp(i0, L"Default (80 Mbit/s)") == 0 && SendMessageW(br, CB_GETCOUNT, 0, 0) == 9 && SendMessageW(br, CB_GETCURSEL, 0, 0) == 0,
                  "Codec AV1: the Bitrate drop-down's first item reads 'Default (80 Mbit/s)', still selected");
        }
        SendMessageW(br, CB_SETCURSEL, 3, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_BITRATE, CBN_SELCHANGE), (LPARAM)br);
        Check(GetEdit(d, settings::ID_BITRATEHINT) == L"about 113 GB per hour", "Bitrate 250 Mbit/s: hint 'about 113 GB per hour'");
    }
    // free space of the output folder's drive, under the folder field
    {
        const std::wstring fs = GetEdit(d, settings::ID_OUTFREE);
        printf("    free space line: '%s'\n", N(fs).c_str());
        Check(OnTab(GetDlgItem(d, settings::ID_OUTFREE)) && fs.size() > 3 && fs[1] == L':' && fs.find(L" free") != std::wstring::npos,
              "free-space line shows '<drive>: <n> GB free' for the output folder");
        const std::wstring keep = GetEdit(d, settings::ID_OUTDIR);
        SetEdit(d, settings::ID_OUTDIR, L"Q:\\no-such-drive\\x");   // EN_CHANGE -> refreshed once typing pauses (400 ms debounce)
        Check(GetEdit(d, settings::ID_OUTFREE) == fs, "free-space line is not re-queried per keystroke (debounced)");
        Sleep(800);
        const std::wstring fs2 = GetEdit(d, settings::ID_OUTFREE);
        printf("    free space line for Q:\\no-such-drive\\x: '%s'\n", N(fs2).c_str());
        const bool qExists = GetFileAttributesW(L"Q:\\") != INVALID_FILE_ATTRIBUTES;
        Check(fs2 != fs && (qExists || fs2 == L"free space unknown"), "free-space line follows the folder field after the pause (unknown drive -> 'free space unknown')");
        SetEdit(d, settings::ID_OUTDIR, keep.c_str());
        Sleep(800);
        Check(GetEdit(d, settings::ID_OUTFREE) == fs, "free-space line restored with the original folder");
    }
    // "Output layout" drop-down (Export group, right above Eye crop): greyed unless Projection is flat. The scratch config starts as vr180.
    {
        RECT lr{}, er{}, yr{};
        GetWindowRect(GetDlgItem(d, settings::ID_LAYOUT3D), &lr);
        GetWindowRect(GetDlgItem(d, settings::ID_EYECROP), &er);
        GetWindowRect(GetDlgItem(d, settings::ID_YTMODE), &yr);
        const HWND ll = ChildByText(d, L"Output layout:");
        RECT llr{};
        if (ll) GetWindowRect(ll, &llr);
        printf("    Output layout combo at (%ld,%ld), Eye crop at (%ld,%ld), YouTube output at (%ld,%ld)\n", lr.left, lr.top, er.left, er.top,
               yr.left, yr.top);
        Check(OnTab(GetDlgItem(d, settings::ID_LAYOUT3D)) && lr.left == er.left && er.top - lr.top == 30 && lr.left == yr.left &&
                  lr.top > yr.top && ll && llr.top > lr.top - 8 && llr.bottom < lr.bottom + 8,
              "Output layout (combo + label) sits in the Export group, in the YouTube output column, one row above Eye crop");
    }
    Check(!IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "Output layout drop-down is greyed while Projection is vr180 (loaded from the file)");
    // Dome fill / Fade after (and their labels): only for VR180 / VR360 -> enabled with vr180, greyed with flat
    const HWND fillL = ChildByText(d, L"Dome fill:"), fadeL = ChildByText(d, L"Fade after:");
    auto domeFields = [&](bool on) {
        return (IsWindowEnabled(GetDlgItem(d, settings::ID_FILL)) != FALSE) == on && (IsWindowEnabled(GetDlgItem(d, settings::ID_FILLFADE)) != FALSE) == on &&
               fillL && fadeL && (IsWindowEnabled(fillL) != FALSE) == on && (IsWindowEnabled(fadeL) != FALSE) == on;
    };
    Check(domeFields(true), "Dome fill / Fade after (drop-down, edit, labels) are enabled while Projection is vr180");
    const HWND ryt = GetDlgItem(d, settings::ID_RECORDYT);
    Check(ryt && !IsWindowEnabled(ryt), "'Record directly in the YouTube layout' is greyed while Projection is vr180 (dome ignores it)");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 0, 0);          // flat
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_PROJECTION));
    Check(IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "Output layout drop-down is enabled once Projection is changed to flat");
    Check(domeFields(false), "Dome fill / Fade after (drop-down, edit, labels) are greyed once Projection is flat");
    Check(ryt && IsWindowEnabled(ryt), "'Record directly in the YouTube layout' is enabled for flat");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 2, 0);          // vr360
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_PROJECTION));
    Check(!IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "Output layout drop-down is greyed again for vr360");
    Check(domeFields(true), "Dome fill / Fade after are enabled again for vr360");
    SendMessageW(GetDlgItem(d, settings::ID_PROJECTION), CB_SETCURSEL, 0, 0);          // flat
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_PROJECTION));
    Check(IsWindowEnabled(GetDlgItem(d, settings::ID_LAYOUT3D)), "Output layout drop-down is enabled for flat again");
    Check(domeFields(false), "Dome fill / Fade after are greyed for flat again");
    SendMessageW(GetDlgItem(d, settings::ID_SWAP), BM_SETCHECK, BST_CHECKED, 0);
    // "Include overlay layers" check (record_overlays): loaded checked from the file, unticked -> record_overlays = 0
    {
        const HWND ovc = GetDlgItem(d, settings::ID_OVERLAYS);
        wchar_t txt[96] = L"";
        GetWindowTextW(ovc, txt, 96);
        printf("    Include overlay layers check: '%s', %s\n", N(txt).c_str(),
               SendMessageW(ovc, BM_GETCHECK, 0, 0) == BST_CHECKED ? "checked" : "unchecked");
        Check(ovc != nullptr && OnTab(ovc) && SendMessageW(ovc, BM_GETCHECK, 0, 0) == BST_CHECKED &&
                  wcscmp(txt, L"Include overlay layers (menus, mod panels)") == 0,
              "Include overlay layers check shows record_overlays = 1 from the file");
        SendMessageW(ovc, BM_SETCHECK, BST_UNCHECKED, 0);
    }
    // "Controllers:" drop-down on the Hotkey row (vr_hotkey): 5 items, loads thumbsticks as "Both stick clicks", right of the hotkey edit
    {
        const HWND vh = GetDlgItem(d, settings::ID_VRHOTKEY), he = GetDlgItem(d, settings::ID_HOTKEY);
        wchar_t txt[96] = L"";
        const LRESULT vsel = vh ? SendMessageW(vh, CB_GETCURSEL, 0, 0) : -1;
        if (vsel >= 0) SendMessageW(vh, CB_GETLBTEXT, (WPARAM)vsel, (LPARAM)txt);
        RECT rv{}, rh{};
        if (vh) GetWindowRect(vh, &rv);
        if (he) GetWindowRect(he, &rh);
        printf("    Controllers drop-down: '%s', %d items\n", N(txt).c_str(), vh ? (int)SendMessageW(vh, CB_GETCOUNT, 0, 0) : -1);
        Check(vh && OnTab(vh) && SendMessageW(vh, CB_GETCOUNT, 0, 0) == 5 && wcscmp(txt, L"Both stick clicks") == 0 &&
                  rv.left > rh.right && rv.top < rh.bottom && rv.bottom > rh.top,
              "Controllers drop-down (vr_hotkey) shows 'Both stick clicks' on the Hotkey row, right of the hotkey field");
    }
    // "YouTube output" drop-down: item 2 = "3D MKV and 8K 3D MKV" (post_mkv = 1 and post_yt8k = 1; run_settings.ps1 checks the saved file)
    // "8K bitrate" drop-down (yt8k_bitrate_mbps): greyed while YouTube output is "3D MKV" (loaded), enabled with 8K; loaded 150 -> pick 200
    const HWND y8 = GetDlgItem(d, settings::ID_YT8KBITRATE);
    Check(y8 && !IsWindowEnabled(y8), "8K bitrate drop-down is greyed while YouTube output has no 8K file");
    const HWND y8c = GetDlgItem(d, settings::ID_YT8KCODEC);
    Check(y8c && OnTab(y8c) && !IsWindowEnabled(y8c), "8K codec drop-down exists and is greyed while YouTube output has no 8K file");
    const HWND y8l = ChildByText(d, L"8K bitrate:"), y8cl = ChildByText(d, L"8K codec:");
    Check(y8l && y8cl && !IsWindowEnabled(y8l) && !IsWindowEnabled(y8cl), "'8K bitrate:' / '8K codec:' labels are greyed with their drop-downs");
    SendMessageW(GetDlgItem(d, settings::ID_YTMODE), CB_SETCURSEL, 2, 0);
    SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_YTMODE, CBN_SELCHANGE), (LPARAM)GetDlgItem(d, settings::ID_YTMODE));
    {
        wchar_t item[64] = L"";
        const int sel = (int)SendMessageW(y8, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(y8, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    8K bitrate drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(y8, CB_GETCOUNT, 0, 0), sel, N(item).c_str());
        Check(IsWindowEnabled(y8) && SendMessageW(y8, CB_GETCOUNT, 0, 0) == 3 && sel == 1 && wcscmp(item, L"150 Mbit/s (default)") == 0,
              "8K bitrate drop-down enabled with 8K output, shows yt8k_bitrate_mbps = 150; picking 200");
        SendMessageW(y8, CB_SETCURSEL, 2, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_YT8KBITRATE, CBN_SELCHANGE), (LPARAM)y8);
        // "8K codec" (yt8k_codec): enabled with 8K output too, shows hevc = "HEVC (default)"; pick AV1
        wchar_t citem[64] = L"";
        const int csel = (int)SendMessageW(y8c, CB_GETCURSEL, 0, 0);
        if (csel >= 0) SendMessageW(y8c, CB_GETLBTEXT, (WPARAM)csel, (LPARAM)citem);
        printf("    8K codec drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(y8c, CB_GETCOUNT, 0, 0), csel, N(citem).c_str());
        Check(IsWindowEnabled(y8c) && SendMessageW(y8c, CB_GETCOUNT, 0, 0) == 2 && csel == 0 && wcscmp(citem, L"HEVC (default)") == 0,
              "8K codec drop-down enabled with 8K output, shows yt8k_codec = hevc as 'HEVC (default)'; picking AV1");
        SendMessageW(y8c, CB_SETCURSEL, 1, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_YT8KCODEC, CBN_SELCHANGE), (LPARAM)y8c);
        Check(IsWindowEnabled(y8l) && IsWindowEnabled(y8cl), "'8K bitrate:' / '8K codec:' labels are enabled with 8K output (flat)");
        // VR180: the 8K step is skipped for dome recordings -> both drop-downs and labels greyed; back to flat -> enabled
        const HWND pj = GetDlgItem(d, settings::ID_PROJECTION);
        SendMessageW(pj, CB_SETCURSEL, 1, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)pj);
        Check(!IsWindowEnabled(y8) && !IsWindowEnabled(y8c) && !IsWindowEnabled(y8l) && !IsWindowEnabled(y8cl),
              "8K bitrate / codec drop-downs and labels are greyed for VR180 (the 8K step skips dome recordings)");
        SendMessageW(pj, CB_SETCURSEL, 0, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_PROJECTION, CBN_SELCHANGE), (LPARAM)pj);
        Check(IsWindowEnabled(y8) && IsWindowEnabled(y8c) && IsWindowEnabled(y8l) && IsWindowEnabled(y8cl),
              "8K bitrate / codec enabled again for flat");
        // "Record directly in the YouTube layout" (record_for_youtube): loaded 0 (unchecked); ticked -> the 8K bitrate / codec
        // drop-downs and labels are greyed (the recording already is the 8K file); left ticked: Save writes record_for_youtube = 1
        wchar_t rt[96] = L"";
        GetWindowTextW(ryt, rt, 96);
        printf("    Record for YouTube check: '%s', %s\n", N(rt).c_str(), SendMessageW(ryt, BM_GETCHECK, 0, 0) == BST_CHECKED ? "checked" : "unchecked");
        Check(OnTab(ryt) && SendMessageW(ryt, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
                  wcscmp(rt, L"Record directly in the YouTube layout (no re-encode)") == 0,
              "'Record directly in the YouTube layout (no re-encode)' check exists and shows record_for_youtube = 0 from the file");
        {
            RECT rr{}, er{}, kr{};
            GetWindowRect(ryt, &rr);
            GetWindowRect(GetDlgItem(d, settings::ID_EYECROP), &er);
            GetWindowRect(GetDlgItem(d, settings::ID_KEEPMP4), &kr);
            Check(rr.top > er.top && rr.left == er.left && kr.left == er.left, "the check sits in the Export group, below Eye crop");
        }
        SendMessageW(ryt, BM_CLICK, 0, 0);
        Check(SendMessageW(ryt, BM_GETCHECK, 0, 0) == BST_CHECKED, "Record for YouTube ticked");
        Check(!IsWindowEnabled(y8) && !IsWindowEnabled(y8c) && !IsWindowEnabled(y8l) && !IsWindowEnabled(y8cl),
              "Record for YouTube ticked: 8K bitrate / codec drop-downs and labels are greyed (no 8K step)");
        UpdateWindow(GetDlgItem(d, settings::ID_LAYOUTPREVIEW));   // the preview shows the recording frame: repaint must not crash
        SendMessageW(ryt, BM_CLICK, 0, 0);
        Check(IsWindowEnabled(y8) && IsWindowEnabled(y8c) && IsWindowEnabled(y8l) && IsWindowEnabled(y8cl),
              "Record for YouTube cleared: 8K bitrate / codec enabled again");
        SendMessageW(ryt, BM_CLICK, 0, 0);   // ticked for the Save below (record_for_youtube 0->1)
        Check(SendMessageW(ryt, BM_GETCHECK, 0, 0) == BST_CHECKED && !IsWindowEnabled(y8), "Record for YouTube ticked again for the save");
    }
    // "Output layout" drop-down: item 1 = side by side, half width (layout_3d = sbs_half)
    SendMessageW(GetDlgItem(d, settings::ID_LAYOUT3D), CB_SETCURSEL, 1, 0);
    // "Convert" drop-down (post_when): loaded as item 2 "When the game closes"; item 0 = "Immediately" (now)
    {
        const HWND pw = GetDlgItem(d, settings::ID_POSTWHEN);
        wchar_t item[64] = L"";
        const int sel = (int)SendMessageW(pw, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(pw, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    Convert drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(pw, CB_GETCOUNT, 0, 0), sel, N(item).c_str());
        Check(pw != nullptr && SendMessageW(pw, CB_GETCOUNT, 0, 0) == 4 && sel == 2 && wcscmp(item, L"When the game exits") == 0,
              "Convert drop-down has 4 items and shows post_when = game_exit from the file ('When the game exits')");
        wchar_t last[64] = L"";
        SendMessageW(pw, CB_GETLBTEXT, 3, (LPARAM)last);
        Check(wcscmp(last, L"When I say") == 0, "Convert drop-down item 3 is 'When I say' (post_when = manual)");
        SendMessageW(pw, CB_SETCURSEL, 0, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_POSTWHEN, CBN_SELCHANGE), (LPARAM)pw);
    }
    // "Eye crop" drop-down (eye_crop): loaded as item 0 "16:9 (recommended)"; item 1 = 4:3, 2 = 4:3pad, 3 = none
    {
        const HWND ec = GetDlgItem(d, settings::ID_EYECROP);
        wchar_t item[128] = L"";
        const int sel = (int)SendMessageW(ec, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(ec, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    Eye crop drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(ec, CB_GETCOUNT, 0, 0), sel, N(item).c_str());
        Check(ec != nullptr && SendMessageW(ec, CB_GETCOUNT, 0, 0) == 4 && sel == 0 && wcscmp(item, L"16:9 (recommended)") == 0,
              "Eye crop drop-down shows eye_crop = 16:9 from the file");
        wchar_t padItem[128] = L"", noneItem[128] = L"";
        SendMessageW(ec, CB_GETLBTEXT, 2, (LPARAM)padItem);
        SendMessageW(ec, CB_GETLBTEXT, 3, (LPARAM)noneItem);
        Check(wcscmp(padItem, L"4:3, padded to 16:9 (8K tier, black side bars)") == 0 && wcscmp(noneItem, L"None (full eye)") == 0,
              "Eye crop item 2 = '4:3, padded to 16:9 ...' (4:3pad), item 3 = 'None (full eye)'");
        SendMessageW(ec, CB_SETCURSEL, 2, 0);   // the preview repaints with the bars: must not crash
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_EYECROP, CBN_SELCHANGE), (LPARAM)ec);
        UpdateWindow(GetDlgItem(d, settings::ID_LAYOUTPREVIEW));
        SendMessageW(ec, CB_SETCURSEL, 1, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_EYECROP, CBN_SELCHANGE), (LPARAM)ec);
    }
    // "Resolution" drop-down (max_width): the scratch file has max_width = 3000, none of the items -> an extra
    // "Custom (3000 px)" item is selected; left untouched, Save keeps 3000 (checked after the window reopens)
    {
        const HWND mw = GetDlgItem(d, settings::ID_MAXWIDTH);
        wchar_t item[96] = L"";
        const int sel = (int)SendMessageW(mw, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(mw, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    Resolution drop-down: %d items, selected %d '%s'\n", (int)SendMessageW(mw, CB_GETCOUNT, 0, 0), sel, N(item).c_str());
        Check(mw != nullptr && OnTab(mw) && SendMessageW(mw, CB_GETCOUNT, 0, 0) == 5 && sel == 4 &&
                  wcscmp(item, L"Custom (3000 px)") == 0,
              "Resolution drop-down shows max_width = 3000 from the file as 'Custom (3000 px)'");
    }
    // "SteamVR games..." footer button: exists, text not cut (the tray's text fit check covers it too), enabled (the tray set the hook),
    // opens the SteamVR games window of the SAME process; a second click brings the same window to the front (still one window).
    {
        const HWND sb = GetDlgItem(d, settings::ID_OPENSTEAMVR);
        wchar_t t[64] = L"";
        if (sb) GetWindowTextW(sb, t, 64);
        Check(sb != nullptr && OnTab(sb) && IsWindowEnabled(sb) && wcscmp(t, L"SteamVR games...") == 0,
              "'SteamVR games...' button exists, visible and enabled");
        if (sb) {
            RECT br{}, wr2{};
            GetWindowRect(sb, &br);
            GetWindowRect(d, &wr2);
            Check(br.right <= wr2.right && br.bottom <= wr2.bottom && br.right - br.left >= 88, "'SteamVR games...' button lies inside the window");
        }
        Check(FindTop(pid, steamvr_ui::kWindowClass, steamvr_ui::kWindowTitle) == nullptr, "no SteamVR games window before the click");
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_OPENSTEAMVR, BN_CLICKED), (LPARAM)sb);
        HWND sv = nullptr;
        for (int i = 0; i < 30 && !sv; ++i) { sv = FindTop(pid, steamvr_ui::kWindowClass, steamvr_ui::kWindowTitle); if (!sv) Sleep(100); }
        Check(sv != nullptr, "clicking 'SteamVR games...' opens the window 'VR EyeCapture - SteamVR games' in the tray");
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_OPENSTEAMVR, BN_CLICKED), (LPARAM)sb);
        Sleep(300);
        int count = 0;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            wchar_t c[64] = L"";
            GetClassNameW(h, c, 64);
            if (wcscmp(c, steamvr_ui::kWindowClass) == 0) ++*(int*)lp;
            return TRUE;
        }, (LPARAM)&count);
        Check(count == 1 && FindTop(pid, steamvr_ui::kWindowClass, steamvr_ui::kWindowTitle) == sv, "a second click keeps one SteamVR games window (the same one)");
        if (sv) {
            SendMessageW(sv, WM_CLOSE, 0, 0);
            for (int i = 0; i < 30 && IsWindow(sv); ++i) Sleep(100);
            Check(!IsWindow(sv), "the SteamVR games window closes");
        }
    }
    // "Convert a recording..." button (Export group) and the "Conversions" group (queue / progress view). The scratch tray has
    // nothing queued: idle status, empty list, Pause / Cancel / Start / Start all / Remove greyed. (Clicking the button would open a
    // file dialog, so it is only checked to exist and be enabled.)
    {
        const HWND cp = GetDlgItem(d, settings::ID_CONVERTPICK);
        wchar_t t[64] = L"";
        if (cp) GetWindowTextW(cp, t, 64);
        Check(cp != nullptr && OnTab(cp) && IsWindowEnabled(cp) && wcscmp(t, L"Convert a recording...") == 0,
              "'Convert a recording...' button exists, visible and enabled");
        const HWND aq = GetDlgItem(d, settings::ID_ADDQUEUE);
        wchar_t at[64] = L"";
        if (aq) GetWindowTextW(aq, at, 64);
        Check(aq != nullptr && OnTab(aq) && IsWindowEnabled(aq) && wcscmp(at, L"Add to queue...") == 0,
              "'Add to queue...' button exists, visible and enabled");
        wchar_t st[32] = L"", sa[32] = L"";
        GetWindowTextW(GetDlgItem(d, settings::ID_QSTART), st, 32);
        GetWindowTextW(GetDlgItem(d, settings::ID_QSTARTALL), sa, 32);
        Check(wcscmp(st, L"Start") == 0 && wcscmp(sa, L"Start all") == 0, "queue view: 'Start' and 'Start all' buttons");
        Check((GetWindowLongW(GetDlgItem(d, settings::ID_QLIST), GWL_STYLE) & LBS_EXTENDEDSEL) != 0, "queue view: the waiting list is multi-select");
        Sleep(700);   // one queue-view refresh (500 ms timer)
        const HWND qs = GetDlgItem(d, settings::ID_QSTATUS), qp = GetDlgItem(d, settings::ID_QPROGRESS), ql = GetDlgItem(d, settings::ID_QLIST);
        const std::wstring status = GetEdit(d, settings::ID_QSTATUS);
        wchar_t first[128] = L"";
        if (ql && SendMessageW(ql, LB_GETCOUNT, 0, 0) > 0) SendMessageW(ql, LB_GETTEXT, 0, (LPARAM)first);
        printf("    queue view: status '%s', list %d item(s), first '%s'\n", N(status).c_str(), ql ? (int)SendMessageW(ql, LB_GETCOUNT, 0, 0) : -1,
               N(first).c_str());
        Check(qs && OnTab(qs) && status.compare(0, 22, L"Nothing is converting.") == 0, "queue view: status says 'Nothing is converting.'");
        Check(qp && OnTab(qp) && SendMessageW(qp, PBM_GETPOS, 0, 0) == 0, "queue view: progress bar exists at 0");
        Check(ql && OnTab(ql) && SendMessageW(ql, LB_GETCOUNT, 0, 0) == 1 && wcscmp(first, L"(no recordings waiting)") == 0,
              "queue view: waiting list exists and is empty ('(no recordings waiting)')");
        bool greyed = true;
        for (int id : {settings::ID_QPAUSE, settings::ID_QCANCEL, settings::ID_QSTART, settings::ID_QSTARTALL, settings::ID_QREMOVE}) {
            const HWND bt = GetDlgItem(d, id);
            if (!bt || !OnTab(bt) || IsWindowEnabled(bt)) greyed = false;
        }
        Check(greyed, "queue view: Pause / Cancel / Start / Start all / Remove exist and are greyed with nothing queued");
        RECT wr{};
        GetWindowRect(d, &wr);
        // A window taller than the work area is shrunk to it and scrolls (WS_VSCROLL): its height then says nothing about the
        // content. The unscrolled height = the scroll range (content + footer) + the frame.
        RECT cr{}, wa{};
        GetClientRect(d, &cr);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const bool scrolls = (GetWindowLongW(d, GWL_STYLE) & WS_VSCROLL) != 0;
        long fullH = wr.bottom - wr.top;
        if (scrolls) {
            SCROLLINFO si{sizeof(si), SIF_RANGE};
            if (GetScrollInfo(d, SB_VERT, &si)) fullH = (si.nMax + 1) + (wr.bottom - wr.top) - (cr.bottom - cr.top);
        }
        printf("    window size %ldx%ld (client height %ld, work area height %ld, %s; unscrolled height %ld)\n", wr.right - wr.left,
               wr.bottom - wr.top, cr.bottom - cr.top, wa.bottom - wa.top, scrolls ? "scrolls" : "no scroll bar", fullH);
        Check(cr.bottom - cr.top <= wa.bottom - wa.top, "the client area fits the work area");
        if (wa.bottom - wa.top >= 1032) Check(!scrolls, "no scroll bar on a work area at least 1032 high (the content fits without scrolling)");
        Check(fullH <= 1032 && wr.right - wr.left <= 1920, "the window fits a 1920x1080 screen with a taskbar (unscrolled height <= 1032)");
    }
    if (!leaveSlider) SendMessageW(GetDlgItem(d, settings::ID_SMOOTH), TBM_SETPOS, TRUE, 7);   // 0.35 ("noslider": untouched keeps the file text)
    Check(SendMessageW(GetDlgItem(d, settings::ID_AUTOSTART), BM_GETCHECK, 0, 0) == BST_UNCHECKED, "Start with Windows: unchecked by default");
    SendMessageW(GetDlgItem(d, settings::ID_AUTOSTART), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(d, settings::ID_SAVE), BM_CLICK, 0, 0);
    Sleep(300);
    Check(FindDialog(pid) != nullptr, "good values: Save saved and kept the window open");
    {
        wchar_t st[32] = L"";
        GetWindowTextW(GetDlgItem(d, settings::ID_SAVE), st, 32);
        Check(wcscmp(st, L"Saved") == 0, "the Save button reads 'Saved' right after the save");
    }
    SendMessageW(GetDlgItem(d, settings::ID_CANCEL), BM_CLICK, 0, 0);   // Exit: nothing unsaved, closes at once
    Check(WaitDialog(pid, false) == nullptr, "Exit closed the window");
    std::wstring stored;
    Check(install::AutostartEnabled(&stored), "Start with Windows ticked + Save: the (test) Run value exists");
    Check(stored.size() > 2 && stored.front() == L'"' && stored.find(L"EyeCaptureTray.exe") != std::wstring::npos, "Run value is a quoted EyeCaptureTray.exe path");
    printf("    Run value: %s\n", N(stored).c_str());
    {   // the user's tick writes the Run value, never the StartupApproved record (Windows' own switch)
        HKEY ak = nullptr;
        bool rec = false;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test\\RunApproved", 0, KEY_QUERY_VALUE, &ak) == ERROR_SUCCESS) {
            rec = RegQueryValueExW(ak, install::AutostartValueName(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
            RegCloseKey(ak);
        }
        Check(!rec, "Start with Windows ticked + Save: no StartupApproved record was written");
    }
    { std::wstring e; install::SetAutostart(false, L"", e); }
    // Resolution round trip through the window: reopen -> still Custom (3000) -> pick 4K -> Save; reopen -> 4K (the
    // custom item is gone) -> pick Native -> Save; reopen -> Native -> Cancel. run_settings.ps1 checks the file / tray.log.
    struct Pick { int wantCount, wantSel; const wchar_t* wantText; int pick; const char* what; };
    const Pick picks[] = {
        {5, 4, L"Custom (3000 px)", 1, "reopened: max_width = 3000 was kept by Save (Custom item); picking 4K"},
        {4, 1, L"3840 px (4K)", 0, "reopened: max_width = 3840 shows as 4K, no Custom item; picking Native"},
        {4, 0, L"Native (full resolution)", -1, "reopened: max_width = 0 shows as Native; Cancel"},
    };
    for (const Pick& pk : picks) {
        PostMessageW(c.found, WM_APP + 3, 0, 0);
        HWND d2 = WaitDialog(pid, true);
        Check(d2 != nullptr, "app window reopened");
        if (!d2) break;
        const HWND mw = GetDlgItem(d2, settings::ID_MAXWIDTH);
        wchar_t item[96] = L"";
        const int sel = (int)SendMessageW(mw, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(mw, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        const int count = (int)SendMessageW(mw, CB_GETCOUNT, 0, 0);
        printf("    Resolution drop-down: %d items, selected %d '%s'\n", count, sel, N(item).c_str());
        Check(count == pk.wantCount && sel == pk.wantSel && wcscmp(item, pk.wantText) == 0, pk.what);
        if (pk.pick >= 0) {
            SendMessageW(mw, CB_SETCURSEL, (WPARAM)pk.pick, 0);
            SendMessageW(d2, WM_COMMAND, MAKEWPARAM(settings::ID_MAXWIDTH, CBN_SELCHANGE), (LPARAM)mw);
            SendMessageW(GetDlgItem(d2, settings::ID_SAVE), BM_CLICK, 0, 0);
            SendMessageW(GetDlgItem(d2, settings::ID_CANCEL), BM_CLICK, 0, 0);   // Save keeps the window open: Exit
        } else {
            SendMessageW(GetDlgItem(d2, settings::ID_CANCEL), BM_CLICK, 0, 0);
        }
        Check(WaitDialog(pid, false) == nullptr, "the window closed");
        Sleep(300);
    }
    // post_when = manual through the window: reopen -> "Immediately" (saved above) -> pick "When I say" -> Save; reopen ->
    // "When I say" is selected -> Cancel. run_settings.ps1 checks the file (post_when = manual) and tray.log.
    for (int round = 0; round < 2; ++round) {
        PostMessageW(c.found, WM_APP + 3, 0, 0);
        HWND d3 = WaitDialog(pid, true);
        Check(d3 != nullptr, "app window reopened");
        if (!d3) break;
        const HWND pw = GetDlgItem(d3, settings::ID_POSTWHEN);
        wchar_t item[64] = L"";
        const int sel = (int)SendMessageW(pw, CB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(pw, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)item);
        printf("    Convert drop-down: selected %d '%s'\n", sel, N(item).c_str());
        if (round == 0) {
            Check(sel == 0 && wcscmp(item, L"Immediately") == 0, "reopened: post_when = now shows as 'Immediately'; picking 'When I say'");
            wchar_t bi[64] = L"", yi[64] = L"";
            const int bs = (int)SendMessageW(GetDlgItem(d3, settings::ID_BITRATE), CB_GETCURSEL, 0, 0);
            const int ys = (int)SendMessageW(GetDlgItem(d3, settings::ID_YT8KBITRATE), CB_GETCURSEL, 0, 0);
            if (bs >= 0) SendMessageW(GetDlgItem(d3, settings::ID_BITRATE), CB_GETLBTEXT, (WPARAM)bs, (LPARAM)bi);
            if (ys >= 0) SendMessageW(GetDlgItem(d3, settings::ID_YT8KBITRATE), CB_GETLBTEXT, (WPARAM)ys, (LPARAM)yi);
            Check(wcscmp(bi, L"250 Mbit/s") == 0 && wcscmp(yi, L"200 Mbit/s") == 0,
                  "reopened: bitrate 250 Mbit/s and 8K bitrate 200 Mbit/s were saved and are shown");
            wchar_t ci[64] = L"", yc[64] = L"";
            const int cs = (int)SendMessageW(GetDlgItem(d3, settings::ID_CODEC), CB_GETCURSEL, 0, 0);
            const int ycs = (int)SendMessageW(GetDlgItem(d3, settings::ID_YT8KCODEC), CB_GETCURSEL, 0, 0);
            if (cs >= 0) SendMessageW(GetDlgItem(d3, settings::ID_CODEC), CB_GETLBTEXT, (WPARAM)cs, (LPARAM)ci);
            if (ycs >= 0) SendMessageW(GetDlgItem(d3, settings::ID_YT8KCODEC), CB_GETLBTEXT, (WPARAM)ycs, (LPARAM)yc);
            printf("    reopened: codec '%s', 8K codec '%s'\n", N(ci).c_str(), N(yc).c_str());
            Check(wcscmp(ci, L"AV1 (hardware: RTX 40 / 50)") == 0 && wcscmp(yc, L"AV1 (RTX 40 / 50)") == 0,
                  "reopened: codec AV1 and 8K codec AV1 were saved and are shown");
            SendMessageW(pw, CB_SETCURSEL, 3, 0);
            SendMessageW(d3, WM_COMMAND, MAKEWPARAM(settings::ID_POSTWHEN, CBN_SELCHANGE), (LPARAM)pw);
            SendMessageW(GetDlgItem(d3, settings::ID_SAVE), BM_CLICK, 0, 0);
            SendMessageW(GetDlgItem(d3, settings::ID_CANCEL), BM_CLICK, 0, 0);   // Save keeps the window open: Exit
        } else {
            Check(sel == 3 && wcscmp(item, L"When I say") == 0, "reopened: post_when = manual shows as 'When I say'; Cancel");
            SendMessageW(GetDlgItem(d3, settings::ID_CANCEL), BM_CLICK, 0, 0);
        }
        Check(WaitDialog(pid, false) == nullptr, "the window closed");
        Sleep(300);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test\\Run");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test\\RunApproved");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\OpenXR-EyeCapture-Test");   // only succeeds when empty
    install::SetAutostartTestKey(L"");
    return g_fail;
}
// settings_test queue <tray pid> <action> [args]: drives the Conversions part of the REAL window of a scratch tray (run_hold.ps1 H).
// Opens the window (WM_APP+3), does one action, prints the waiting list ("ITEM: <text>") and the status ("STATUS: ..."), then
// closes the window with Cancel (nothing is saved: the queued jobs keep the values the window showed). The scratch tray reads
// the "picked" files from EYECAP_TRAY_TEST_PICK instead of opening a file dialog.
//   add <eye crop item 0..2> <keep mp4 0|1>   set Eye crop + Keep original MP4 in the window (unsaved), click "Add to queue..."
//   convert <eye crop item> <keep mp4 0|1>      the same with "Convert a recording..." (starts at once)
//   start <list index>                           select that row, click "Start"
//   startall                                     click "Start all"
//   list                                         only print
HWND TrayWindow(DWORD pid) {
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, L"OpenXREyeCaptureTray", nullptr)) != nullptr;) {
        DWORD wp = 0;
        GetWindowThreadProcessId(w, &wp);
        if (wp == pid) return w;   // never the user's own tray
    }
    return nullptr;
}
void DumpQueue(HWND d) {
    const HWND ql = GetDlgItem(d, settings::ID_QLIST);
    const int n = ql ? (int)SendMessageW(ql, LB_GETCOUNT, 0, 0) : 0;
    for (int i = 0; i < n; ++i) {
        wchar_t t[512] = L"";
        if (SendMessageW(ql, LB_GETTEXTLEN, (WPARAM)i, 0) < 511) SendMessageW(ql, LB_GETTEXT, (WPARAM)i, (LPARAM)t);
        printf("ITEM: %s\n", N(t).c_str());
    }
    std::wstring st = GetEdit(d, settings::ID_QSTATUS);
    for (size_t p = 0; (p = st.find(L"\r\n", p)) != std::wstring::npos;) st.replace(p, 2, L" | ");
    printf("STATUS: %s\n", N(st).c_str());
    printf("BUTTONS: start=%d startall=%d remove=%d\n", IsWindowEnabled(GetDlgItem(d, settings::ID_QSTART)) ? 1 : 0,
           IsWindowEnabled(GetDlgItem(d, settings::ID_QSTARTALL)) ? 1 : 0, IsWindowEnabled(GetDlgItem(d, settings::ID_QREMOVE)) ? 1 : 0);
}
int TestQueue(DWORD pid, int argc, wchar_t** argv) {
    g_fail = 0;
    const HWND tw = TrayWindow(pid);
    Check(tw != nullptr, "found the tray's message window");
    if (!tw || argc < 4) return 1;
    const std::wstring action = argv[3];
    PostMessageW(tw, WM_APP + 3, 0, 0);
    HWND d = WaitDialog(pid, true);
    Check(d != nullptr, "app window opened");
    if (!d) return 1;
    Sleep(700);   // one queue-view refresh
    if ((action == L"add" || action == L"convert") && argc >= 6) {
        const HWND ec = GetDlgItem(d, settings::ID_EYECROP);
        SendMessageW(ec, CB_SETCURSEL, (WPARAM)_wtoi(argv[4]), 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_EYECROP, CBN_SELCHANGE), (LPARAM)ec);
        SendMessageW(GetDlgItem(d, settings::ID_KEEPMP4), BM_SETCHECK, _wtoi(argv[5]) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(d, action == L"add" ? settings::ID_ADDQUEUE : settings::ID_CONVERTPICK), BM_CLICK, 0, 0);
        printf("clicked %s (eye crop item %d, keep mp4 %d)\n", action == L"add" ? "Add to queue..." : "Convert a recording...", _wtoi(argv[4]),
               _wtoi(argv[5]));
    } else if (action == L"start" && argc >= 5) {
        const HWND ql = GetDlgItem(d, settings::ID_QLIST);
        SendMessageW(ql, LB_SETSEL, FALSE, -1);
        SendMessageW(ql, LB_SETSEL, TRUE, (LPARAM)_wtoi(argv[4]));
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(settings::ID_QLIST, LBN_SELCHANGE), (LPARAM)ql);
        Check(IsWindowEnabled(GetDlgItem(d, settings::ID_QSTART)) != FALSE, "Start is enabled with a waiting row selected");
        SendMessageW(GetDlgItem(d, settings::ID_QSTART), BM_CLICK, 0, 0);
        printf("clicked Start on row %d\n", _wtoi(argv[4]));
    } else if (action == L"startall") {
        Check(IsWindowEnabled(GetDlgItem(d, settings::ID_QSTARTALL)) != FALSE, "Start all is enabled");
        SendMessageW(GetDlgItem(d, settings::ID_QSTARTALL), BM_CLICK, 0, 0);
        printf("clicked Start all\n");
    } else if (action != L"list") {
        printf("unknown queue action\n");
        ++g_fail;
    }
    Sleep(700);
    DumpQueue(d);
    SendMessageW(GetDlgItem(d, settings::ID_CANCEL), BM_CLICK, 0, 0);
    Check(WaitDialog(pid, false) == nullptr, "the window closed");
    return g_fail;
}
// ---- settings_test svr: the "SteamVR games" window
struct SvrFind { DWORD pid; const wchar_t* cls; const wchar_t* title; HWND found; };
HWND FindTop(DWORD pid, const wchar_t* cls, const wchar_t* title) {
    SvrFind f{pid, cls, title, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* x = (SvrFind*)lp;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p != x->pid || !IsWindowVisible(h)) return TRUE;
        wchar_t c[64] = L"", t[128] = L"";
        GetClassNameW(h, c, 64);
        GetWindowTextW(h, t, 128);
        if (wcscmp(c, x->cls) == 0 && wcscmp(t, x->title) == 0) { x->found = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&f);
    return f.found;
}
std::string OneLine(std::wstring s) {
    for (size_t p = 0; (p = s.find(L"\r\n", p)) != std::wstring::npos;) s.replace(p, 2, L" / ");
    for (auto& c : s) if (c == L'\n') c = L'|'; else if (c == L'\t') c = L'~';
    return N(s);
}
// The message box's buttons by their text without '&' ("Yes", "No", "OK").
HWND BoxButton(HWND box, const std::wstring& want) {
    struct Ctx { std::wstring want; HWND found; } ctx{want, nullptr};
    EnumChildWindows(box, [](HWND c, LPARAM lp) -> BOOL {
        auto* x = (Ctx*)lp;
        wchar_t cls[32] = L"", t[64] = L"";
        GetClassNameW(c, cls, 32);
        if (_wcsicmp(cls, L"GlassButton") != 0) return TRUE;   // tray/glass.h kButton
        GetWindowTextW(c, t, 64);
        std::wstring s;
        for (const wchar_t* p = t; *p; ++p) if (*p != L'&') s += *p;
        if (_wcsicmp(s.c_str(), x->want.c_str()) == 0) { x->found = c; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}
void DumpSvr(HWND d) {
    const HWND lb = GetDlgItem(d, steamvr_ui::ID_SVR_LIST);
    const int n = lb ? (int)SendMessageW(lb, LB_GETCOUNT, 0, 0) : 0;
    for (int i = 0; i < n; ++i) {
        wchar_t t[1024] = L"";
        if (SendMessageW(lb, LB_GETTEXTLEN, (WPARAM)i, 0) < 1023) SendMessageW(lb, LB_GETTEXT, (WPARAM)i, (LPARAM)t);
        std::wstring s = t;
        for (size_t p = 0; (p = s.find(L'\t', p)) != std::wstring::npos;) s.replace(p, 1, L" | ");
        printf("ROW: %s\n", N(s).c_str());
    }
    wchar_t st[1024] = L"";
    SendMessageW(GetDlgItem(d, steamvr_ui::ID_SVR_STATUS), WM_GETTEXT, 1024, (LPARAM)st);
    printf("STATUS: %s\n", N(st).c_str());
    printf("MODE: %d\n", (int)SendMessageW(GetDlgItem(d, steamvr_ui::ID_SVR_MODE), CB_GETCURSEL, 0, 0));
    wchar_t det[8192] = L"";
    SendMessageW(GetDlgItem(d, steamvr_ui::ID_SVR_DETAILS), WM_GETTEXT, 8192, (LPARAM)det);   // (GetDlgItemText reads nothing cross-process here)
    printf("DETAILS: %s\n", OneLine(det).c_str());
    printf("BUTTONS: enableall=%d disableall=%d ", IsWindowEnabled(GetDlgItem(d, steamvr_ui::ID_SVR_ENABLEALL)) ? 1 : 0,
           IsWindowEnabled(GetDlgItem(d, steamvr_ui::ID_SVR_DISABLEALL)) ? 1 : 0);
    printf("enable=%d disable=%d reenable=%d repair=%d\n", IsWindowEnabled(GetDlgItem(d, steamvr_ui::ID_SVR_ENABLE)) ? 1 : 0,
           IsWindowEnabled(GetDlgItem(d, steamvr_ui::ID_SVR_DISABLE)) ? 1 : 0, IsWindowEnabled(GetDlgItem(d, steamvr_ui::ID_SVR_REENABLE)) ? 1 : 0,
           IsWindowEnabled(GetDlgItem(d, steamvr_ui::ID_SVR_REPAIR)) ? 1 : 0);
}
int TestSvr(DWORD pid, int argc, wchar_t** argv) {
    g_fail = 0;
    const HWND tw = TrayWindow(pid);
    Check(tw != nullptr, "found the tray's message window");
    if (!tw || argc < 4) return 1;
    const std::wstring action = argv[3];
    std::wstring rowName;
    int modeSel = -1;
    bool viaBalloon = false;
    std::vector<std::wstring> answers;
    for (int i = 4; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a.compare(0, 4, L"row=") == 0) rowName = a.substr(4);
        else if (a == L"via=balloon") viaBalloon = true;
        else if (a.compare(0, 5, L"mode=") == 0) modeSel = _wtoi(a.c_str() + 5);
        else if (a.compare(0, 8, L"answers=") == 0) {
            std::wstring rest = a.substr(8);
            for (size_t p = 0; p <= rest.size();) {
                size_t c = rest.find(L',', p);
                if (c == std::wstring::npos) c = rest.size();
                if (c > p) answers.push_back(rest.substr(p, c - p));
                p = c + 1;
            }
        }
    }
    if (action == L"unregister") {   // the tray menu's "Unregister from VR games..." (WM_APP+11); no window
        PostMessageW(tw, WM_APP + 11, 0, 0);
        Sleep(3000);
        printf("POSTED: WM_APP+11\n");
        return g_fail;
    }
    if (viaBalloon) PostMessageW(tw, WM_APP + 1, 1, NIN_BALLOONUSERCLICK);   // WM_TRAY: a click on the last balloon
    else PostMessageW(tw, WM_APP + 9, 0, 0);
    HWND d = nullptr;
    for (int i = 0; i < 100 && !d; ++i) { d = FindTop(pid, steamvr_ui::kWindowClass, steamvr_ui::kWindowTitle); if (!d) Sleep(50); }
    Check(d != nullptr, viaBalloon ? "SteamVR games window opened by the balloon click" : "SteamVR games window opened");
    if (!d) return 1;
    auto waitScan = [&] {
        for (int i = 0; i < 300; ++i) {
            wchar_t st[1024] = L"";
            SendMessageW(GetDlgItem(d, steamvr_ui::ID_SVR_STATUS), WM_GETTEXT, 1024, (LPARAM)st);
            if (!wcsstr(st, L"Scanning")) return;
            Sleep(50);
        }
        Check(false, "the Steam scan finished within 15 s");
    };
    Sleep(200);
    waitScan();
    Sleep(200);
    if (!rowName.empty()) {
        const HWND lb = GetDlgItem(d, steamvr_ui::ID_SVR_LIST);
        const int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
        int row = -1;
        for (int i = 0; i < n && row < 0; ++i) {
            wchar_t t[1024] = L"";
            if (SendMessageW(lb, LB_GETTEXTLEN, (WPARAM)i, 0) < 1023) SendMessageW(lb, LB_GETTEXT, (WPARAM)i, (LPARAM)t);
            if (_wcsnicmp(t, rowName.c_str(), rowName.size()) == 0 && t[rowName.size()] == L'\t') row = i;
        }
        Check(row >= 0, "the game's row is in the list");
        SendMessageW(lb, LB_SETCURSEL, (WPARAM)row, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(steamvr_ui::ID_SVR_LIST, LBN_SELCHANGE), (LPARAM)lb);
    }
    int button = 0;
    bool acted = false;
    if (action == L"enable") button = steamvr_ui::ID_SVR_ENABLE;
    else if (action == L"enableall") button = steamvr_ui::ID_SVR_ENABLEALL;
    else if (action == L"disable") button = steamvr_ui::ID_SVR_DISABLE;
    else if (action == L"reenable") button = steamvr_ui::ID_SVR_REENABLE;
    else if (action == L"repair") button = steamvr_ui::ID_SVR_REPAIR;
    else if (action == L"disableall") button = steamvr_ui::ID_SVR_DISABLEALL;
    else if (action == L"rescan") button = steamvr_ui::ID_SVR_RESCAN;
    else if (action == L"toggle") {
        PostMessageW(d, WM_COMMAND, MAKEWPARAM(steamvr_ui::ID_SVR_TOGGLE, 0), 0);
        acted = true;
    } else if (action == L"click") {   // a real click into the selected row's check box column
        const HWND lb = GetDlgItem(d, steamvr_ui::ID_SVR_LIST);
        const int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0), top = (int)SendMessageW(lb, LB_GETTOPINDEX, 0, 0);
        const int ih = (int)SendMessageW(lb, LB_GETITEMHEIGHT, 0, 0);
        const LPARAM pt = MAKELPARAM(8, (sel - top) * ih + ih / 2);
        PostMessageW(lb, WM_LBUTTONDOWN, MK_LBUTTON, pt);
        PostMessageW(lb, WM_LBUTTONUP, 0, pt);
        acted = true;
    } else if (action == L"mode") {   // the "Recording mode" drop-down (steamvr_mode), item mode=<n>
        const HWND cb = GetDlgItem(d, steamvr_ui::ID_SVR_MODE);
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)modeSel, 0);
        SendMessageW(d, WM_COMMAND, MAKEWPARAM(steamvr_ui::ID_SVR_MODE, CBN_SELCHANGE), (LPARAM)cb);
        Sleep(200);
    } else if (action != L"list") { printf("unknown svr action\n"); ++g_fail; }
    if (button) {
        Check(IsWindowEnabled(GetDlgItem(d, button)) != FALSE, "the button is enabled");
        // posted, not sent: the click opens modal message boxes that this test has to answer
        PostMessageW(d, WM_COMMAND, MAKEWPARAM(button, BN_CLICKED), (LPARAM)GetDlgItem(d, button));
        acted = true;
    }
    if (acted) {
        size_t next = 0;
        ULONGLONG idleSince = GetTickCount64();
        while (GetTickCount64() - idleSince < 2000) {
            const HWND box = FindTop(pid, L"GlassMessage", steamvr_ui::kWindowTitle);   // glass::MessageBox (tray/glass.h)
            if (!box) { Sleep(100); continue; }
            Sleep(150);   // fully created
            wchar_t text[8192] = L"";
            GetDlgItemTextW(box, 0xFFFF, text, 8192);
            printf("BOX: %s\n", OneLine(text).c_str());
            std::wstring ans = next < answers.size() ? answers[next++] : std::wstring();
            if (ans.empty()) ans = BoxButton(box, L"OK") ? L"OK" : L"No";
            const HWND b = BoxButton(box, ans);
            printf("ANSWER: %s\n", N(ans).c_str());
            Check(b != nullptr, "the message box has the answer's button");
            if (!b) { PostMessageW(box, WM_CLOSE, 0, 0); ++g_fail; break; }
            PostMessageW(box, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(b), BN_CLICKED), (LPARAM)b);
            for (int i = 0; i < 50 && IsWindow(box); ++i) Sleep(50);
            idleSince = GetTickCount64();
        }
        Check(next == answers.size(), "every given answer was used (one message box each)");
        Sleep(200);
        waitScan();
    }
    DumpSvr(d);
    PostMessageW(d, WM_COMMAND, MAKEWPARAM(steamvr_ui::ID_SVR_CLOSE, BN_CLICKED), (LPARAM)GetDlgItem(d, steamvr_ui::ID_SVR_CLOSE));
    bool closed = false;
    for (int i = 0; i < 100 && !closed; ++i) { closed = FindTop(pid, steamvr_ui::kWindowClass, steamvr_ui::kWindowTitle) == nullptr; if (!closed) Sleep(50); }
    Check(closed, "the window closed");
    return g_fail;
}
} // namespace
// pp::PredictLayout (the arithmetic behind the conversion's log line AND the window's layout preview) against the numbers the tray logged
// for a Quest 3 recording ("input 8192x4630 = eyes 4096x4630; eye_crop 16:9 -> centre-cropped to 4096x2304; tb_half frame 7680x4320")
// and docs/DEVELOPMENT.md (4:3 -> 5760x4320, none -> 4320x4884).
int TestLayout() {
    g_fail = 0;
    const pp::LayoutSize a = pp::PredictLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Wide169, 4320);
    Check(a.cw == 4096 && a.ch == 2304, "tb_half 16:9: eye cropped to 4096x2304");
    Check(a.fw == 7680 && a.fh == 4320, "tb_half 16:9 8K: frame 7680x4320");
    Check(a.sw == 7680 && a.sh == 2160, "tb_half 16:9 8K: each eye 7680x2160");
    const pp::LayoutSize b = pp::PredictLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Std43, 4320);
    Check(b.fw == 5760 && b.fh == 4320, "tb_half 4:3 8K: frame 5760x4320");
    const pp::LayoutSize c = pp::PredictLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::None, 4320);
    Check(c.fw == 4320 && c.fh == 4884, "tb_half none 8K: frame 4320x4884");
    const pp::LayoutSize d = pp::PredictLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Wide169, 0);
    Check(d.fw == 4096 && d.fh == 2304, "tb_half 16:9 native: frame 4096x2304");
    const pp::LayoutSize e = pp::PredictLayout(4096, 4630, Layout3d::SbsFull, EyeCrop::None, 0);
    Check(e.fw == 8192 && e.fh == 4630, "sbs_full none native: the recording itself, 8192x4630");
    // 4:3pad: the 4:3 crop padded to 16:9 in eye units (5462 wide, rounded) -> 7680x4320 with the 5760 px picture centred
    const pp::LayoutSize f = pp::PredictLayout(4096, 4630, Layout3d::TbHalf, EyeCrop::Pad43, 4320);
    Check(f.cw == 4096 && f.ch == 3072 && f.pw == 5462, "tb_half 4:3pad: crop 4096x3072, padded to 5462x3072");
    Check(f.fw == 7680 && f.fh == 4320 && f.sw == 7680 && f.sh == 2160, "tb_half 4:3pad 8K: frame 7680x4320, each eye 7680x2160");
    Check(f.vw == 5760 && f.vx >= 958 && f.vx <= 962 && f.vx + f.vw <= f.sw, "tb_half 4:3pad 8K: picture 5760 wide at x ~960");
    Check(b.pw == b.cw && b.vw == b.sw && b.vx == 0 && a.vw == a.sw && a.vx == 0, "4:3 / 16:9: no bars (picture = eye)");
    const pp::LayoutSize g = pp::PredictLayout(4096, 2000, Layout3d::TbHalf, EyeCrop::Pad43, 4320);
    const pp::LayoutSize h = pp::PredictLayout(4096, 2000, Layout3d::TbHalf, EyeCrop::Wide169, 4320);
    Check(g.cw == h.cw && g.ch == h.ch && g.fw == h.fw && g.fh == h.fh && g.sw == h.sw && g.sh == h.sh && g.pw == g.cw && g.vx == 0,
          "4:3pad, eye already wider than 16:9: neither cropped nor padded (= 16:9's numbers)");
    // frame-rate rule for the 60 fps YouTube video: 60 / 120 / 180 / 240 and 30 / 20 / 15 are even (+-1.5 fps), the rest is not
    Check(!pp::UnevenFor60(60) && !pp::UnevenFor60(120) && !pp::UnevenFor60(180) && !pp::UnevenFor60(240) &&
          !pp::UnevenFor60(30) && !pp::UnevenFor60(20) && !pp::UnevenFor60(15), "fps 15 / 20 / 30 / 60 / 120 / 180 / 240 are even");
    Check(!pp::UnevenFor60(59.2) && !pp::UnevenFor60(61.5) && !pp::UnevenFor60(118.6) && !pp::UnevenFor60(0) && !pp::UnevenFor60(-1),
          "fps within +-1.5 of an even rate, 0 (unknown) and negative are not flagged");
    Check(pp::UnevenFor60(72) && pp::UnevenFor60(80) && pp::UnevenFor60(90) && pp::UnevenFor60(144) && pp::UnevenFor60(45) &&
          pp::UnevenFor60(62) && pp::UnevenFor60(100) && pp::UnevenFor60(48), "fps 72 / 80 / 90 / 144 / 45 / 62 / 100 / 48 are uneven");
    Check(pp::FpsWarningText(90).find(L"90 fps") == 0 && pp::FpsWarningText(120).empty() && pp::FpsWarningText(0).empty(),
          "the warning text starts with the rate and is empty for an even / unknown rate");
    return g_fail;
}
int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"layout") == 0) return TestLayout() ? 1 : 0;
    if (argc >= 2 && wcscmp(argv[1], L"hotkeys") == 0) return TestHotkeys() ? 1 : 0;
    if (argc >= 3 && wcscmp(argv[1], L"save") == 0) return TestSave(argv[2]) ? 1 : 0;
    if (argc >= 3 && wcscmp(argv[1], L"drive") == 0) return TestDrive((DWORD)_wtoi(argv[2]), argc >= 4 && wcscmp(argv[3], L"noslider") == 0) ? 1 : 0;
    if (argc >= 4 && wcscmp(argv[1], L"queue") == 0) return TestQueue((DWORD)_wtoi(argv[2]), argc, argv) ? 1 : 0;
    if (argc >= 4 && wcscmp(argv[1], L"svr") == 0) return TestSvr((DWORD)_wtoi(argv[2]), argc, argv) ? 1 : 0;
    printf("usage: settings_test hotkeys | layout | save <config.ini> | drive <tray pid> [noslider] | queue <tray pid> <action> [args] | svr <tray pid> <action> [row=..] [answers=..]\n");
    return 2;
}
