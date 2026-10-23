// vr_hotkey_test -- CPU unit test of the VR controller hotkey (src/vr_hotkey: vr_hotkey grammar, controller profile paths,
// the hold detector), its config keys (vr_hotkey, vr_hotkey_hold_s) and of the recording file names that are never reused
// (src/capture_common: two recordings within one second, existing / converted files). No GPU, no OpenXR runtime.
// Exit code 0 = all checks passed.
#include <windows.h>
#include <cstdio>
#include <string>
#include "capture_common.h"
#include "config.h"
#include "vr_hotkey.h"

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
std::string N(const std::wstring& w) { return elog::Narrow(w); }
bool Parses(const wchar_t* t, const wchar_t* canonical) {
    VrHotkey h;
    return ParseVrHotkey(t, h) && h.text == canonical;
}
bool Refused(const wchar_t* t) {
    VrHotkey h;
    h.text = L"unchanged";
    return !ParseVrHotkey(t, h) && h.text == L"unchanged";
}
void Touch(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
} // namespace

int main() {
    printf("--- grammar\n");
    {
        const VrHotkey d = DefaultVrHotkey();
        Check(d.enabled && d.text == L"thumbsticks" && d.buttons.size() == 2 && d.buttons[0].hand == VrHand::Left &&
                  d.buttons[0].button == VrButton::Thumbstick && d.buttons[1].hand == VrHand::Right,
              "default: thumbsticks = left_thumbstick + right_thumbstick");
        VrHotkey off;
        Check(ParseVrHotkey(L" Off ", off) && !off.enabled && off.buttons.empty() && off.text == L"off", "'off' (any case / spaces) disables");
        Check(Parses(L"left_menu+right_a", L"left_menu+right_a"), "left_menu+right_a");
        Check(Parses(L"LEFT_Menu + Right_A", L"left_menu+right_a"), "case and spaces ignored");
        Check(Parses(L"right_thumbstick+left_thumbstick", L"thumbsticks"), "both sticks in any order -> 'thumbsticks'");
        Check(Parses(L"left_squeeze", L"left_grip"), "squeeze is an alias of grip; one button is allowed");
        Check(Parses(L"left_x+left_y+right_a+right_b", L"left_x+left_y+right_a+right_b"), "four buttons");
        Check(Refused(L"left_x+left_y+right_a+right_b+left_menu"), "five buttons refused");
        Check(Refused(L"left_a+left_a"), "the same button twice refused");
        Check(Refused(L"middle_a"), "unknown hand refused");
        Check(Refused(L"left_c"), "unknown button refused");
        Check(Refused(L""), "empty refused");
        Check(Refused(L"left_a+"), "trailing '+' refused");
    }
    printf("--- profile paths\n");
    {
        std::string p;
        const std::string touch = "/interaction_profiles/oculus/touch_controller";
        Check(VrButtonPath(touch, {VrHand::Left, VrButton::Thumbstick}, p) && p == "/user/hand/left/input/thumbstick/click",
              "touch: left_thumbstick -> /user/hand/left/input/thumbstick/click");
        Check(VrButtonPath(touch, {VrHand::Right, VrButton::A}, p) && p == "/user/hand/right/input/a/click", "touch: right_a");
        Check(!VrButtonPath(touch, {VrHand::Right, VrButton::Menu}, p), "touch: no right_menu (the reserved system button)");
        Check(!VrButtonPath(touch, {VrHand::Left, VrButton::A}, p), "touch: no left_a");
        Check(VrButtonPath("/interaction_profiles/htc/vive_controller", {VrHand::Left, VrButton::Thumbstick}, p) &&
                  p == "/user/hand/left/input/trackpad/click", "vive: thumbstick = trackpad click");
        Check(!VrButtonPath("/interaction_profiles/khr/simple_controller", {VrHand::Left, VrButton::Thumbstick}, p),
              "simple controller: no thumbstick");
        Check(VrButtonPath("/interaction_profiles/khr/simple_controller", {VrHand::Right, VrButton::Trigger}, p) &&
                  p == "/user/hand/right/input/select/click", "simple controller: trigger = select/click");
        Check(VrButtonPath("/interaction_profiles/valve/index_controller", {VrHand::Left, VrButton::A}, p), "index: a on both hands");
        Check(!VrButtonPath("/interaction_profiles/unknown/x", {VrHand::Left, VrButton::A}, p), "unknown profile: no path");
        bool all = true;
        for (const char* prof : VrKnownProfiles()) {
            for (VrHand h : {VrHand::Left, VrHand::Right})
                for (int b = 0; b < 8; ++b)
                    if (VrButtonPath(prof, {h, (VrButton)b}, p) && p.compare(0, 11, "/user/hand/") != 0) all = false;
        }
        Check(all && VrKnownProfiles().size() >= 10, "every known profile's paths are under /user/hand/");
        const std::string our = "/user/hand/left/input/thumbstick/click";
        Check(VrSameInput("/user/hand/left/input/thumbstick", our), "same input: the identifier alone");
        Check(VrSameInput("/user/hand/left/input/thumbstick/click", our), "same input: identical path");
        Check(VrSameInput("/user/hand/left/input/thumbstick/x", our), "same input: another component of the stick");
        Check(!VrSameInput("/user/hand/left/input/thumbstick_2/click", our), "not the same: a longer identifier");
        Check(!VrSameInput("/user/hand/right/input/thumbstick/click", our), "not the same: the other hand");
    }
    printf("--- hold detector\n");
    {
        VrHoldTracker t;
        Check(!t.Update(true, 10.0, 1.0) && t.Holding(), "pressed: holding, not yet");
        Check(!t.Update(true, 10.9, 1.0), "0.9 s: not yet");
        Check(t.Update(true, 11.0, 1.0), "1.0 s: fires");
        Check(!t.Update(true, 15.0, 1.0), "still held: fires only once");
        Check(!t.Update(false, 15.1, 1.0) && !t.Holding(), "released: re-armed");
        Check(!t.Update(true, 16.0, 1.0), "pressed again");
        Check(!t.Update(false, 16.5, 1.0), "released after 0.5 s: nothing");
        Check(!t.Update(true, 17.0, 1.0) && t.Update(true, 18.2, 1.0), "held again for 1.2 s: fires");
    }
    printf("--- config keys\n");
    {
        Config c;
        Check(c.vrHotkey.text == L"thumbsticks" && c.vrHotkeyHoldS == 1.0f, "Config defaults: vr_hotkey thumbsticks, vr_hotkey_hold_s 1.0");
        ApplyConfigOverrides(L"vr_hotkey=left_menu+right_a;vr_hotkey_hold_s=2.5", c);
        Check(c.vrHotkey.text == L"left_menu+right_a" && c.vrHotkeyHoldS == 2.5f, "overrides: left_menu+right_a, 2.5 s");
        ApplyConfigOverrides(L"vr_hotkey=left_q;vr_hotkey_hold_s=0.1", c);
        Check(c.vrHotkey.text == L"left_menu+right_a" && c.vrHotkeyHoldS == 2.5f, "bad values (left_q, 0.1 s) keep the previous ones");
        ApplyConfigOverrides(L"vr_hotkey=off", c);
        Check(!c.vrHotkey.enabled, "vr_hotkey = off");
    }
    printf("--- recording file names are never reused\n");
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring dir = std::wstring(tmp) + L"eyecap_vr_hotkey_test_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(dir.c_str(), nullptr);
        // two names within one second (retry if the clock ticked into the next second between the two calls)
        std::wstring a, b;
        for (int i = 0; i < 5; ++i) {
            SYSTEMTIME s0, s1;
            GetLocalTime(&s0);
            a = RecordingFileName(L"game", DomeProjection::Flat, dir);
            b = RecordingFileName(L"game", DomeProjection::Flat, dir);
            GetLocalTime(&s1);
            if (s0.wSecond == s1.wSecond) break;
        }
        printf("    %s, %s\n", N(a).c_str(), N(b).c_str());
        Check(a != b, "two recordings in the same second get different names");
        Check(a.size() > 8 && a.compare(a.size() - 8, 8, L"_SBS.mp4") == 0 && b.compare(b.size() - 10, 10, L"_2_SBS.mp4") == 0,
              "the second one is <stem>_2_SBS.mp4 (the tag stays last)");
        // a file of the next name already exists (another process / an earlier run): skipped
        std::wstring c, d;
        for (int i = 0; i < 5; ++i) {
            SYSTEMTIME s0, s1;
            GetLocalTime(&s0);
            wchar_t stamp[64];
            swprintf_s(stamp, L"other_%04u%02u%02u_%02u%02u%02u", s0.wYear, s0.wMonth, s0.wDay, s0.wHour, s0.wMinute, s0.wSecond);
            Touch(dir + L"\\" + stamp + L"_VR180.mp4");
            Touch(dir + L"\\" + stamp + L"_2_VR180.mkv");   // a converted recording whose MP4 was deleted (keep_mp4 = 0)
            c = RecordingFileName(L"other", DomeProjection::Vr180, dir);
            GetLocalTime(&s1);
            d = std::wstring(stamp) + L"_3_VR180.mp4";
            if (s0.wSecond == s1.wSecond) break;
        }
        printf("    existing %s: picked %s\n", "<stem>_VR180.mp4 + <stem>_2_VR180.mkv", N(c).c_str());
        Check(c == d, "an existing MP4 and a converted <stem>_2 .mkv are both skipped -> <stem>_3_VR180.mp4");
        // clean up
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(dir.c_str());
    }
    {   // ATS / ETS2 suggest only khr/simple_controller: its bindings translated onto the real profiles
        const std::string touch = "/interaction_profiles/oculus/touch_controller", index = "/interaction_profiles/valve/index_controller";
        const std::string vive = "/interaction_profiles/htc/vive_controller";
        std::string o;
        Check(VrTranslateSimpleBinding(touch, "/user/hand/right/input/select/click", o) && o == "/user/hand/right/input/trigger/value",
              "simple select -> touch trigger/value");
        Check(VrTranslateSimpleBinding(index, "/user/hand/left/input/select/click", o) && o == "/user/hand/left/input/trigger/click",
              "simple select -> index trigger/click");
        Check(VrTranslateSimpleBinding(touch, "/user/hand/left/input/menu/click", o) && o == "/user/hand/left/input/menu/click",
              "simple left menu -> touch left menu");
        Check(!VrTranslateSimpleBinding(touch, "/user/hand/right/input/menu/click", o), "simple right menu: none on touch (system button)");
        Check(VrTranslateSimpleBinding(index, "/user/hand/right/input/menu/click", o) && o == "/user/hand/right/input/b/click",
              "simple menu -> index b");
        Check(VrTranslateSimpleBinding(vive, "/user/hand/right/input/menu/click", o) && o == "/user/hand/right/input/menu/click",
              "simple right menu -> vive menu");
        Check(VrTranslateSimpleBinding(touch, "/user/hand/left/input/aim/pose", o) && o == "/user/hand/left/input/aim/pose" &&
              VrTranslateSimpleBinding(touch, "/user/hand/right/output/haptic", o) && o == "/user/hand/right/output/haptic",
              "aim pose / haptic unchanged");
        Check(!VrTranslateSimpleBinding(touch, "/user/head/input/system/click", o), "a non-hand path is left out");
        bool all = true;   // every target is a known profile with thumbsticks (so the default combination works there)
        for (const char* t : VrSimpleTranslationTargets()) {
            std::string p;
            all = all && VrButtonPath(t, {VrHand::Left, VrButton::Thumbstick}, p) && VrButtonPath(t, {VrHand::Right, VrButton::Thumbstick}, p);
        }
        Check(all, "every translation target has both thumbsticks");
    }

    printf("=== vr_hotkey_test %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
