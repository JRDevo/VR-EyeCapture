// EyeCaptureTray -- optional tray control for the OpenXR-EyeCapture layer (Win32, no frameworks).
//
// Tray icon: grey = no game, green = game running (ready), red = recording, amber = starting / finalizing / failed.
// Right-click (or left-click) menu: Start / Stop recording, Open captures folder, Open log, Edit config, Exit.
// Tooltip: game, API, resolution, fps, frames dropped, elapsed. Talks to the layer through src/shared_status.h; the
// layer never depends on this app (the hotkey keeps working without it).
#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <string>
#include "config.h"
#include "shared_status.h"

namespace {
const UINT WM_TRAY = WM_APP + 1;
const UINT kTimer = 1;
enum : UINT { ID_TOGGLE = 100, ID_FOLDER, ID_LOG, ID_CONFIG, ID_EXIT };

HINSTANCE      g_inst;
HWND           g_wnd;
NOTIFYICONDATAW g_nid{};
EyeCapShared*  g_shm = nullptr;
HANDLE         g_map = nullptr;
HICON          g_icons[4] = {};   // 0 grey, 1 green, 2 red, 3 amber
int            g_iconIdx = -1;
EyeCapShared   g_st{};
bool           g_live = false;    // a live owner exists
uint32_t       g_prevState = ECS_NONE;
std::wstring   g_exeDir;

HICON MakeDotIcon(COLORREF c) {
    const int s = 32;
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = s;
    bh.bV5Height = -s;
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    auto* px = (uint32_t*)bits;
    const double cx = (s - 1) / 2.0, r = s / 2.0 - 2;
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            const double d = sqrt((x - cx) * (x - cx) + (y - cx) * (y - cx));
            double a = r + 0.5 - d;                         // anti-aliased edge
            a = a < 0 ? 0 : (a > 1 ? 1 : a);
            const bool ring = d > r - 2.5;                  // dark rim
            const uint32_t rr = ring ? GetRValue(c) / 2 : GetRValue(c);
            const uint32_t gg = ring ? GetGValue(c) / 2 : GetGValue(c);
            const uint32_t bb = ring ? GetBValue(c) / 2 : GetBValue(c);
            const uint32_t al = (uint32_t)(a * 255);
            px[y * s + x] = (al << 24) | ((rr * al / 255) << 16) | ((gg * al / 255) << 8) | (bb * al / 255);
        }
    }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/"));
}

void Refresh() {
    g_live = false;
    if (g_shm && EyeCapReadStatus(g_shm, g_st) && g_st.ownerPid && EyeCapProcessAlive(g_st.ownerPid) &&
        GetTickCount64() - g_st.heartbeatTick < 3000)
        g_live = true;
    const uint32_t state = g_live ? g_st.state : ECS_NONE;
    int idx = 0;
    if (state == ECS_READY) idx = 1;
    else if (state == ECS_RECORDING) idx = 2;
    else if (state == ECS_STARTING || state == ECS_STOPPING || state == ECS_FAILED) idx = 3;

    wchar_t tip[128];
    if (!g_live) {
        swprintf_s(tip, L"OpenXR-EyeCapture: no OpenXR game running");
    } else if (state == ECS_RECORDING) {
        const ULONGLONG el = g_st.recStartTick ? (GetTickCount64() - g_st.recStartTick) / 1000 : 0;
        swprintf_s(tip, L"REC %llu:%02llu  %s (%s)\n%ux%u @%u fps, dropped %llu", el / 60, el % 60, g_st.game, g_st.api,
                   g_st.width, g_st.height, g_st.fps, (unsigned long long)g_st.framesDropped);
    } else if (state == ECS_STOPPING) {
        swprintf_s(tip, L"%s: finalizing the MP4...", g_st.game);
    } else if (state == ECS_FAILED) {
        swprintf_s(tip, L"%s: start failed\n%.80s", g_st.game, g_st.message);
    } else {
        swprintf_s(tip, L"%s (%s): ready\n%s toggles recording", g_st.game, g_st.api, g_st.hotkey);
    }
    g_nid.uFlags = NIF_TIP | NIF_ICON;
    wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
    g_nid.hIcon = g_icons[idx];
    // balloon when a recording finishes
    if (g_live && g_prevState == ECS_STOPPING && state == ECS_READY && g_st.message[0]) {
        g_nid.uFlags |= NIF_INFO;
        wcsncpy_s(g_nid.szInfoTitle, L"Recording saved", _TRUNCATE);
        wcsncpy_s(g_nid.szInfo, g_st.message, _TRUNCATE);
        g_nid.dwInfoFlags = NIIF_INFO;
    }
    g_prevState = state;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_iconIdx = idx;
}

void OpenPath(const std::wstring& p, const wchar_t* verb = L"open") {
    if (!p.empty()) ShellExecuteW(nullptr, verb, p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowMenu() {
    Refresh();
    HMENU m = CreatePopupMenu();
    const uint32_t state = g_live ? g_st.state : ECS_NONE;
    const bool rec = state == ECS_RECORDING || state == ECS_STARTING;
    std::wstring label = rec ? L"Stop recording" : L"Start recording";
    if (g_live && g_st.hotkey[0]) label += std::wstring(L"\t") + g_st.hotkey;
    AppendMenuW(m, MF_STRING | ((g_live && state != ECS_STOPPING) ? 0 : MF_GRAYED), ID_TOGGLE, label.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_FOLDER, L"Open captures folder");
    AppendMenuW(m, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(m, MF_STRING, ID_CONFIG, L"Edit config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    const UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    DestroyMenu(m);
    Config cfg;
    switch (cmd) {
    case ID_TOGGLE:
        if (g_shm) EyeCapSendCommand(g_shm, rec ? ECC_STOP : ECC_START);
        break;
    case ID_FOLDER: {
        std::wstring dir = g_live ? g_st.outputDir : L"";
        if (dir.empty()) { LoadConfig(g_exeDir, cfg); dir = cfg.outputDir; }
        CreateDirectoryW(dir.c_str(), nullptr);
        OpenPath(dir, L"explore");
        break;
    }
    case ID_LOG:
        OpenPath(g_live && g_st.logPath[0] ? std::wstring(g_st.logPath) : g_exeDir + L"\\logs\\eye_capture.log");
        break;
    case ID_CONFIG: {
        const std::wstring p = g_live && g_st.configPath[0] ? std::wstring(g_st.configPath) : ConfigPath(g_exeDir);
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case ID_EXIT:
        DestroyWindow(g_wnd);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == s_taskbarCreated) { Shell_NotifyIconW(NIM_ADD, &g_nid); return 0; }
    switch (msg) {
    case WM_TIMER:
        Refresh();
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        return 0;
    case WM_DESTROY:
        KillTimer(h, kTimer);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}
// Command-line mode (no tray): EyeCaptureTray.exe --start | --stop | --toggle | --status
// For scripts, Stream Deck buttons, and tests. Prints one status line; exit code 0 = a live game received the command.
int RunCli(const std::wstring& arg) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto print = [&](const std::wstring& s) {
        if (!out || out == INVALID_HANDLE_VALUE) return;
        const std::string u = elog::Narrow(s + L"\r\n");
        DWORD w = 0;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    HANDLE map = nullptr;
    EyeCapShared* shm = EyeCapOpenShared(&map);
    if (!shm) { print(L"error: shared memory unavailable"); return 3; }
    int rc = 0;
    if (arg != L"--status") {
        const uint32_t cmd = arg == L"--start" ? ECC_START : arg == L"--stop" ? ECC_STOP : ECC_TOGGLE;
        if (!EyeCapSendCommand(shm, cmd)) { print(L"no OpenXR game with a capture-capable session is running"); rc = 1; }
        else Sleep(600);   // let the layer react before printing the status
    }
    EyeCapShared st{};
    const bool live = EyeCapReadStatus(shm, st) && st.ownerPid && EyeCapProcessAlive(st.ownerPid);
    static const wchar_t* names[] = {L"none", L"ready", L"starting", L"recording", L"stopping", L"failed"};
    wchar_t line[900];
    if (!live) {
        swprintf_s(line, L"state=none (no game)");
        if (arg == L"--status") rc = 1;
    } else {
        swprintf_s(line, L"state=%s pid=%u game=%s api=%s size=%ux%u fps=%u frames=%llu dropped=%llu file=%s msg=%s",
                   st.state < 6 ? names[st.state] : L"?", st.ownerPid, st.game, st.api, st.width, st.height, st.fps,
                   (unsigned long long)st.framesWritten, (unsigned long long)st.framesDropped, st.file, st.message);
    }
    print(line);
    UnmapViewOfFile(shm);
    CloseHandle(map);
    return rc;
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && cmdLine[0] == L'-') {
        std::wstring a = cmdLine;
        while (!a.empty() && a.back() == L' ') a.pop_back();
        if (a == L"--start" || a == L"--stop" || a == L"--toggle" || a == L"--status") return RunCli(a);
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpenXR-EyeCapture-Tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_inst = inst;
    g_exeDir = ExeDir();
    g_shm = EyeCapOpenShared(&g_map);
    g_icons[0] = MakeDotIcon(RGB(140, 140, 140));
    g_icons[1] = MakeDotIcon(RGB(40, 190, 70));
    g_icons[2] = MakeDotIcon(RGB(230, 30, 30));
    g_icons[3] = MakeDotIcon(RGB(240, 170, 20));

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"OpenXREyeCaptureTray";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, L"OpenXR-EyeCapture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icons[0];
    wcsncpy_s(g_nid.szTip, L"OpenXR-EyeCapture", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    SetTimer(g_wnd, kTimer, 500, nullptr);
    Refresh();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (HICON i : g_icons) if (i) DestroyIcon(i);
    if (g_shm) UnmapViewOfFile(g_shm);
    if (g_map) CloseHandle(g_map);
    if (single) CloseHandle(single);
    return 0;
}
