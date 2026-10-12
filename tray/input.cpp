// input -- see input.h.
#include "input.h"
#include <windows.h>
#include <dbt.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <Xinput.h>
#include <atomic>
#include <cwctype>
#include <memory>
#include <mutex>
#include "log.h"

namespace input {
namespace {

// ------------------------------------------------------------------------------------------------ shared state
std::mutex                                   g_lock;          // guards everything in this block
struct BindState { HotkeyBinding b; bool active = false; ULONGLONG lastFire = 0; };
std::vector<BindState>                       g_bindings;
std::vector<DeviceInfo>                      g_devices;
std::wstring                                 g_captured;
bool                                         g_hasCaptured = false;
std::function<void(const HotkeyBinding&)>    g_onFire;

std::atomic<bool>     g_capture{false};
std::atomic<bool>     g_haveBindings{false};   // any Pad / Joy binding: poll every 20 ms (else once a second, for the device log)
std::atomic<unsigned> g_captureGen{0};    // bumped by SetCapture(true): the poll thread takes a new baseline
HANDLE                g_thread = nullptr;
HANDLE                g_stop = nullptr;
HANDLE                g_ready = nullptr;
HANDLE                g_wake = nullptr;   // SetBindings / SetCapture: leave the idle poll interval at once

// ------------------------------------------------------------------------------------------------ poll thread only
// One input source: an XInput slot or a DirectInput device. cur / ignore: [0..3] buttons (pad: [0] = kPad* bits),
// [4] = POV direction bits.
struct Source {
    unsigned cur[5] = {};
    unsigned ignore[5] = {};   // capture: held when capture started (or after a capture) -> ignored until released
    bool seen = false;         // had a good read since it connected / was opened
};
struct Pad : Source {
    bool connected = false;
    ULONGLONG nextProbe = 0;   // a disconnected slot is probed once a second (XInputGetState is slow on empty slots)
};
struct Joy : Source {
    IDirectInputDevice8W* dev = nullptr;
    GUID instance{};
    std::wstring name;         // sanitised product name
    unsigned buttons = 0, povs = 0;
    bool skipped = false;      // XInput device: listed, not read
    bool dropped = false;      // reads kept failing: ignored until the next enumeration
    int failures = 0;
};

typedef DWORD (WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
struct XStateEx { XINPUT_STATE s; DWORD reserved[4]; };   // XInputGetStateEx may write a little more than XINPUT_STATE
HMODULE           s_xdll = nullptr;
XInputGetStateFn  s_xget = nullptr;
XInputGetStateFn  s_xgetEx = nullptr;
Pad               s_pads[4];
IDirectInput8W*   s_di = nullptr;
std::vector<std::unique_ptr<Joy>> s_joys;
HWND              s_wnd = nullptr;
ULONGLONG         s_enumAt = 0;        // 0 = no re-enumeration pending
ULONGLONG         s_lastEnum = 0;
std::wstring      s_lastDevicesText;
// capture
unsigned          s_capGen = 0;
bool              s_capOn = false;
const Source*     s_capSource = nullptr;
unsigned          s_chord[5] = {};
// A device had its first good read in this cycle: its held buttons are its starting state, not a press (no binding fires
// in that cycle, and capture ignores them until released) -- e.g. an H-shifter that is in gear when it is plugged in.
bool              s_fresh = false;

// First good read of a source: what it holds now is not a press.
void FirstRead(Source& s) {
    if (s.seen) return;
    s.seen = true;
    for (int k = 0; k < 5; ++k) s.ignore[k] = s.cur[k];
    s_fresh = true;
}

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DEVICECHANGE) {
        if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            s_enumAt = GetTickCount64() + 1000;   // debounce: one enumeration after the burst of change messages
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void LoadXInput() {
    for (const wchar_t* n : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        s_xdll = LoadLibraryW(n);
        if (!s_xdll) continue;
        s_xget = (XInputGetStateFn)(void*)GetProcAddress(s_xdll, "XInputGetState");
        s_xgetEx = (XInputGetStateFn)(void*)GetProcAddress(s_xdll, (LPCSTR)100);   // hidden: also reports Guide
        if (s_xget || s_xgetEx) {
            LOGI("tray: input: %s loaded (Guide button %s)", elog::Narrow(n).c_str(), s_xgetEx ? "supported" : "not supported");
            return;
        }
        FreeLibrary(s_xdll);
        s_xdll = nullptr;
    }
    LOGW("tray: input: no XInput DLL found -- Pad: bindings disabled");
}

BOOL CALLBACK EnumCb(LPCDIDEVICEINSTANCEW inst, LPVOID ctx) {
    ((std::vector<DIDEVICEINSTANCEW>*)ctx)->push_back(*inst);
    return DIENUM_CONTINUE;
}

void RebuildDeviceList() {
    std::vector<DeviceInfo> list;
    for (unsigned i = 0; i < 4; ++i) {
        if (!s_pads[i].connected) continue;
        DeviceInfo d;
        d.kind = L"pad";
        d.name = L"XInput pad " + std::to_wstring(i + 1);
        d.index = i + 1;
        d.buttons = s_xgetEx ? 17 : 16;
        list.push_back(d);
    }
    unsigned n = 0;
    for (const auto& j : s_joys) {
        DeviceInfo d;
        d.kind = L"joy";
        d.name = j->name;
        d.index = ++n;
        d.buttons = j->buttons;
        d.povs = j->povs;
        d.xinputSkipped = j->skipped;
        list.push_back(d);
    }
    const std::wstring text = DevicesText(list);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_devices = list;
    }
    if (text != s_lastDevicesText) {
        s_lastDevicesText = text;
        LOGI("tray: devices: %s", elog::Narrow(text).c_str());
    }
}

void ReleaseJoy(Joy& j) {
    if (j.dev) {
        j.dev->Unacquire();
        j.dev->Release();
        j.dev = nullptr;
    }
}

void Enumerate() {
    s_enumAt = 0;
    s_lastEnum = GetTickCount64();
    if (!s_di) { RebuildDeviceList(); return; }
    std::vector<DIDEVICEINSTANCEW> found;
    const HRESULT hr = s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, &found, DIEDFL_ATTACHEDONLY);
    if (FAILED(hr)) LOGW("tray: input: DirectInput EnumDevices failed (0x%08lX)", (unsigned long)hr);
    std::vector<std::unique_ptr<Joy>> next;
    for (const auto& inst : found) {
        // keep a working device as it is (its capture state included)
        bool kept = false;
        for (auto& old : s_joys) {
            if (old && IsEqualGUID(old->instance, inst.guidInstance) && !old->dropped) {
                next.push_back(std::move(old));
                kept = true;
                break;
            }
        }
        if (kept) continue;
        auto j = std::make_unique<Joy>();
        j->instance = inst.guidInstance;
        j->name = SanitizeDeviceName(inst.tszProductName);
        if (j->name.empty()) j->name = SanitizeDeviceName(inst.tszInstanceName);
        if (j->name.empty()) j->name = L"Game controller";
        IDirectInputDevice8W* dev = nullptr;
        if (FAILED(s_di->CreateDevice(inst.guidInstance, &dev, nullptr)) || !dev) {
            LOGW("tray: input: cannot open \"%s\"", elog::Narrow(j->name).c_str());
            continue;
        }
        DIPROPGUIDANDPATH gp{};
        gp.diph.dwSize = sizeof(gp);
        gp.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        gp.diph.dwHow = DIPH_DEVICE;
        if (SUCCEEDED(dev->GetProperty(DIPROP_GUIDANDPATH, &gp.diph)) && Upper(gp.wszPath).find(L"IG_") != std::wstring::npos) {
            dev->Release();
            j->skipped = true;   // an XInput device: read through XInput as a pad
            next.push_back(std::move(j));
            continue;
        }
        DIDEVCAPS caps{};
        caps.dwSize = sizeof(caps);
        HRESULT h2 = dev->SetDataFormat(&c_dfDIJoystick2);
        if (SUCCEEDED(h2)) h2 = dev->SetCooperativeLevel(s_wnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        if (SUCCEEDED(h2) && SUCCEEDED(dev->GetCapabilities(&caps))) {
            j->buttons = caps.dwButtons;
            j->povs = caps.dwPOVs;
        }
        if (FAILED(h2)) {
            LOGW("tray: input: \"%s\" cannot be set up for background reading (0x%08lX)", elog::Narrow(j->name).c_str(), (unsigned long)h2);
            dev->Release();
            continue;
        }
        dev->Acquire();   // may fail now; retried on every read
        j->dev = dev;
        next.push_back(std::move(j));
    }
    for (auto& old : s_joys) {
        if (!old) continue;
        if (s_capSource == old.get()) s_capSource = nullptr;
        ReleaseJoy(*old);
    }
    s_joys = std::move(next);
    RebuildDeviceList();
}

void PollPads(ULONGLONG now) {
    if (!s_xget && !s_xgetEx) return;
    bool changed = false;
    for (DWORD i = 0; i < 4; ++i) {
        Pad& p = s_pads[i];
        if (!p.connected && now < p.nextProbe) continue;
        XStateEx st{};
        const DWORD r = s_xgetEx ? s_xgetEx(i, &st.s) : s_xget(i, &st.s);
        const bool conn = r == ERROR_SUCCESS;
        if (conn != p.connected) { p.connected = conn; changed = true; }
        if (!conn) {
            p.nextProbe = now + 1000;
            for (unsigned& c : p.cur) c = 0;
            p.seen = false;
            continue;
        }
        unsigned b = st.s.Gamepad.wButtons & (s_xgetEx ? 0xF7FFu : 0xF3FFu);
        if (st.s.Gamepad.bLeftTrigger > 128) b |= kPadLT;
        if (st.s.Gamepad.bRightTrigger > 128) b |= kPadRT;
        p.cur[0] = b;
        FirstRead(p);
    }
    if (changed) RebuildDeviceList();
}

void PollJoys() {
    for (auto& jp : s_joys) {
        Joy& j = *jp;
        if (!j.dev || j.skipped || j.dropped) continue;
        DIJOYSTATE2 st{};
        HRESULT hr = j.dev->Poll();
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) { j.dev->Acquire(); j.dev->Poll(); }
        hr = j.dev->GetDeviceState(sizeof(st), &st);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            j.dev->Acquire();
            hr = j.dev->GetDeviceState(sizeof(st), &st);
        }
        if (FAILED(hr)) {
            for (unsigned& c : j.cur) c = 0;
            if (++j.failures >= 100) {   // about 2 s of failed reads
                j.dropped = true;
                LOGW("tray: input: \"%s\" stopped answering (0x%08lX) -- ignored until the next device change",
                     elog::Narrow(j.name).c_str(), (unsigned long)hr);
                s_enumAt = GetTickCount64() + 10000;
            }
            continue;
        }
        j.failures = 0;
        unsigned cur[5] = {};
        for (unsigned n = 0; n < 128; ++n)
            if (st.rgbButtons[n] & 0x80) cur[n / 32] |= 1u << (n % 32);
        const DWORD pov = st.rgdwPOV[0];
        if (j.povs > 0 && LOWORD(pov) != 0xFFFF) {
            const unsigned a = (unsigned)(pov % 36000);   // hundredths of a degree, clockwise from up; diagonals set two bits
            if (a > 27000 || a < 9000) cur[4] |= kPovUp;
            if (a > 0 && a < 18000) cur[4] |= kPovRight;
            if (a > 9000 && a < 27000) cur[4] |= kPovDown;
            if (a > 18000) cur[4] |= kPovLeft;
        }
        for (int k = 0; k < 5; ++k) j.cur[k] = cur[k];
        FirstRead(j);
    }
}

// "Press a key" for pads / joysticks. Caller holds g_lock.
void CaptureStep() {
    std::vector<Source*> sources;
    for (Pad& p : s_pads) sources.push_back(&p);
    for (auto& j : s_joys) if (!j->skipped) sources.push_back(j.get());
    const unsigned gen = g_captureGen.load();
    if (!s_capOn || gen != s_capGen) {   // capture (re)started: everything already held is ignored until released
        s_capOn = true;
        s_capGen = gen;
        s_capSource = nullptr;
        for (Source* s : sources) for (int k = 0; k < 5; ++k) s->ignore[k] = s->cur[k];
    }
    for (Source* s : sources) for (int k = 0; k < 5; ++k) s->ignore[k] &= s->cur[k];
    auto eff = [](const Source* s, int k) { return s->cur[k] & ~s->ignore[k]; };
    if (!s_capSource) {
        for (Source* s : sources) {
            bool any = false;
            for (int k = 0; k < 5; ++k) if (eff(s, k)) any = true;
            if (!any) continue;
            s_capSource = s;
            for (int k = 0; k < 5; ++k) s_chord[k] = eff(s, k);
            break;
        }
        return;
    }
    bool released = false;
    for (int k = 0; k < 5; ++k) if ((eff(s_capSource, k) & s_chord[k]) != s_chord[k]) released = true;
    if (!released) {
        for (int k = 0; k < 5; ++k) s_chord[k] |= eff(s_capSource, k);
        return;
    }
    std::wstring text;
    bool ok = false;
    for (unsigned i = 0; i < 4; ++i)
        if (s_capSource == &s_pads[i]) ok = PadBindingText(0, s_chord[0], text);
    for (const auto& j : s_joys)
        if (s_capSource == j.get()) ok = JoyBindingText(j->name, s_chord, s_chord[4], text);
    if (ok) {
        g_captured = text;
        g_hasCaptured = true;
    }
    s_capSource = nullptr;
    for (Source* s : sources) for (int k = 0; k < 5; ++k) s->ignore[k] = s->cur[k];   // the rest of the chord is not a new press
}

bool BindingActive(const HotkeyBinding& b) {
    if (b.kind == HotkeyBinding::Kind::Pad) {
        for (unsigned i = 0; i < 4; ++i) {
            const Pad& p = s_pads[i];
            if (p.connected && (b.padIndex == 0 || b.padIndex == i + 1) && (p.cur[0] & b.padButtons) == b.padButtons) return true;
        }
        return false;
    }
    for (const auto& j : s_joys) {
        if (j->skipped || j->dropped || !j->dev || !JoyDeviceMatches(b.joyDevice, j->name)) continue;
        bool all = (j->cur[4] & b.joyPov) == b.joyPov;
        for (int k = 0; k < 4 && all; ++k) all = (j->cur[k] & b.joyButtons[k]) == b.joyButtons[k];
        if (all) return true;
    }
    return false;
}

// A Joy binding whose device is not connected: look again every 10 s (some devices send no device-change message).
bool JoyDeviceMissing() {
    for (const auto& bs : g_bindings) {
        if (bs.b.kind != HotkeyBinding::Kind::Joy) continue;
        bool found = false;
        for (const auto& j : s_joys) if (!j->skipped && !j->dropped && JoyDeviceMatches(bs.b.joyDevice, j->name)) found = true;
        if (!found) return true;
    }
    return false;
}

void Evaluate(ULONGLONG now) {
    std::vector<HotkeyBinding> fired;
    std::function<void(const HotkeyBinding&)> onFire;
    bool missing = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        const bool capture = g_capture.load();
        if (capture) CaptureStep(); else s_capOn = false;
        for (BindState& bs : g_bindings) {
            const bool act = BindingActive(bs.b);
            if (act && !bs.active && !capture && !s_fresh && now - bs.lastFire >= 300) {
                bs.lastFire = now;
                fired.push_back(bs.b);
            }
            bs.active = act;   // also while capturing: a button still held afterwards does not fire
        }
        onFire = g_onFire;
        missing = JoyDeviceMissing();
    }
    s_fresh = false;
    if (missing && !s_enumAt && now - s_lastEnum >= 10000) s_enumAt = now;
    if (onFire) for (const auto& b : fired) onFire(b);
}

DWORD WINAPI ThreadProc(LPVOID) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"EyeCaptureInputWindow";
    RegisterClassW(&wc);
    // a real (hidden) top-level window: DirectInput's SetCooperativeLevel wants one, and it gets WM_DEVICECHANGE broadcasts
    s_wnd = CreateWindowExW(0, wc.lpszClassName, L"OpenXR-EyeCapture input", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr,
                            nullptr, inst, nullptr);
    HDEVNOTIFY notify = nullptr;
    if (s_wnd) {
        DEV_BROADCAST_DEVICEINTERFACE_W f{};
        f.dbcc_size = sizeof(f);
        f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        f.dbcc_classguid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};   // GUID_DEVINTERFACE_HID
        notify = RegisterDeviceNotificationW(s_wnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    } else {
        LOGW("tray: input: hidden window not created (error %lu) -- Joy: bindings disabled", GetLastError());
    }
    LoadXInput();
    if (s_wnd) {
        const HRESULT hr = DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&s_di, nullptr);
        if (FAILED(hr)) { s_di = nullptr; LOGW("tray: input: DirectInput8Create failed (0x%08lX) -- Joy: bindings disabled", (unsigned long)hr); }
    }
    PollPads(GetTickCount64());
    Enumerate();
    SetEvent(g_ready);

    ULONGLONG nextPoll = GetTickCount64();
    const HANDLE waits[2] = {g_stop, g_wake};
    for (;;) {
        ULONGLONG now = GetTickCount64();
        const DWORD wait = nextPoll > now ? (DWORD)(nextPoll - now) : 0;
        const DWORD r = MsgWaitForMultipleObjects(2, waits, FALSE, wait, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 1) nextPoll = 0;   // bindings / capture changed: poll now
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        now = GetTickCount64();
        if (now < nextPoll) continue;
        // nothing to watch: once a second keeps the device list (and the log) current
        nextPoll = now + (g_haveBindings.load() || g_capture.load() ? 20 : 1000);
        if (s_enumAt && now >= s_enumAt) Enumerate();
        PollPads(now);
        PollJoys();
        Evaluate(now);
    }

    for (auto& j : s_joys) if (j) ReleaseJoy(*j);
    s_joys.clear();
    s_capSource = nullptr;
    if (s_di) { s_di->Release(); s_di = nullptr; }
    if (notify) UnregisterDeviceNotification(notify);
    if (s_wnd) { DestroyWindow(s_wnd); s_wnd = nullptr; }
    UnregisterClassW(wc.lpszClassName, inst);
    if (s_xdll) { FreeLibrary(s_xdll); s_xdll = nullptr; s_xget = s_xgetEx = nullptr; }
    for (Pad& p : s_pads) p = Pad();
    s_lastDevicesText.clear();
    return 0;
}

} // namespace

void Start(std::function<void(const HotkeyBinding&)> onFire) {
    if (g_thread) return;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_onFire = std::move(onFire);
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        LOGE("tray: input: CreateThread failed -- controller / wheel hotkeys disabled");
        CloseHandle(g_stop);
        CloseHandle(g_ready);
        CloseHandle(g_wake);
        g_stop = g_ready = g_wake = nullptr;
    }
}

void Stop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    CloseHandle(g_ready);
    CloseHandle(g_wake);
    g_thread = g_stop = g_ready = g_wake = nullptr;
    std::lock_guard<std::mutex> lk(g_lock);
    g_onFire = nullptr;
    g_devices.clear();
}

bool WaitReady(unsigned timeoutMs) {
    return g_ready && WaitForSingleObject(g_ready, timeoutMs) == WAIT_OBJECT_0;
}

void SetBindings(const std::vector<HotkeyBinding>& all) {
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<BindState> next;
    for (const HotkeyBinding& b : all) {
        if (b.kind == HotkeyBinding::Kind::Key) continue;
        BindState bs;
        bs.b = b;
        bs.active = true;   // a new binding fires only after its buttons were seen released once
        for (const BindState& old : g_bindings)   // same binding as before: keep its state (a held button does not re-fire)
            if (old.b.text == b.text) { bs.active = old.active; bs.lastFire = old.lastFire; }
        next.push_back(bs);
    }
    g_bindings = std::move(next);
    g_haveBindings = !g_bindings.empty();
    if (g_wake) SetEvent(g_wake);
}

void SetCapture(bool on) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (on) {
        g_hasCaptured = false;
        g_captured.clear();
        ++g_captureGen;
    }
    g_capture = on;
    if (g_wake) SetEvent(g_wake);
}

bool TakeCaptured(std::wstring& text) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_hasCaptured) return false;
    text = g_captured;
    g_hasCaptured = false;
    g_captured.clear();
    return true;
}

std::vector<DeviceInfo> Devices() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_devices;
}

std::wstring DevicesText(const std::vector<DeviceInfo>& devices) {
    std::wstring s;
    for (const DeviceInfo& d : devices) {
        if (!s.empty()) s += L"; ";
        if (d.kind == L"pad") {
            s += d.name + L" (" + std::to_wstring(d.buttons) + L" buttons)";
        } else if (d.xinputSkipped) {
            s += L"\"" + d.name + L"\" (XInput device: read as a pad)";
        } else {
            s += L"\"" + d.name + L"\" (" + std::to_wstring(d.buttons) + L" buttons";
            if (d.povs) s += L", " + std::to_wstring(d.povs) + (d.povs == 1 ? L" POV hat" : L" POV hats");
            s += L")";
        }
    }
    return s.empty() ? std::wstring(L"none") : s;
}

} // namespace input
