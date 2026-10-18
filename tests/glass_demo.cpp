// glass_demo -- one of each glass control (tray/glass.h) in a window, for screenshots of the look in light / dark, with
// Mica and solid. Not shipped.
//   glass_demo [--dark | --light] [--solid] [--x N --y N] [--seconds N]
#include <windows.h>
#include <commctrl.h>
#include <cstdlib>
#include <cwchar>
#include "glass.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
HFONT g_font = nullptr;

HWND Make(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, x, y, w, h, p, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

void Build(HWND w) {
    HWND grp = Make(w, glass::kButton, L"Recording", BS_GROUPBOX, 12, 8, 596, 236, -1);
    Make(w, glass::kStatic, L"Hotkey:", SS_LEFT, 28, 40, 110, 18, -1);
    HWND e = Make(w, L"EDIT", L"ScrollLock", WS_TABSTOP | ES_AUTOHSCROLL, 140, 36, 200, 26, 101, WS_EX_CLIENTEDGE);
    glass::FrameEdit(e);
    Make(w, glass::kButton, L"Capture...", WS_TABSTOP | BS_PUSHBUTTON, 350, 35, 100, 28, 102);
    Make(w, glass::kStatic, L"Codec:", SS_LEFT, 28, 76, 110, 18, -1);
    HWND cb = Make(w, glass::kCombo, L"", WS_TABSTOP, 140, 72, 310, 28, 103);
    for (const wchar_t* it : {L"H.265 / HEVC (recommended)", L"H.264 / AVC", L"AV1 (hardware: RTX 40 / 50)"})
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)it);
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
    HWND ck = Make(w, glass::kButton, L"Beep in the headset when recording starts / stops", WS_TABSTOP | BS_AUTOCHECKBOX, 140, 108, 400, 24, 104);
    SendMessageW(ck, BM_SETCHECK, BST_CHECKED, 0);
    Make(w, glass::kButton, L"Include overlay layers (menus, mod panels)", WS_TABSTOP | BS_AUTOCHECKBOX, 140, 136, 400, 24, 105);
    Make(w, glass::kStatic, L"Head smoothing:", SS_LEFT, 28, 174, 110, 18, -1);
    HWND sl = Make(w, glass::kSlider, L"", WS_TABSTOP, 136, 166, 240, 32, 106);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELONG(0, 20));
    SendMessageW(sl, TBM_SETPOS, TRUE, 12);
    HWND hint = Make(w, glass::kStatic, L"0.60 = calm; 0 = off. The dot in the headset is never in the video.", SS_LEFT, 140, 206, 440, 18, -1);
    glass::SetDim(hint, true);
    SetWindowPos(grp, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Make(w, glass::kStatic, L"Outside a card: text straight on the window background.", SS_LEFT, 16, 256, 500, 18, -1);
    HWND ro = Make(w, L"EDIT", L"A read-only multi-line edit (the Description panel):\r\nline two of the help text.", ES_MULTILINE | ES_READONLY, 16, 282, 592, 56, 107);
    glass::FrameEdit(ro);
    Make(w, glass::kButton, L"Restore Defaults", WS_TABSTOP | BS_PUSHBUTTON, 16, 352, 130, 30, 108);
    Make(w, glass::kButton, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 392, 352, 104, 30, 109);
    Make(w, glass::kButton, L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 504, 352, 104, 30, IDOK);
    HWND dis = Make(w, glass::kButton, L"Disabled", WS_TABSTOP | BS_PUSHBUTTON, 160, 352, 100, 30, 110);
    EnableWindow(dis, FALSE);
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        glass::PaintBackground(h, (HDC)wp, rc);
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        return (LRESULT)glass::EditBrush((HDC)wp, (HWND)lp);
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
        glass::OnSettingChange(h);
        return 0;
    case WM_TIMER:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int x = 100, y = 100, seconds = 0;
    for (int i = 1; i < __argc; ++i) {
        const wchar_t* a = __wargv[i];
        if (!wcscmp(a, L"--dark")) glass::ForceDark(1);
        else if (!wcscmp(a, L"--light")) glass::ForceDark(0);
        else if (!wcscmp(a, L"--solid")) glass::ForceMica(0);
        else if (!wcscmp(a, L"--x") && i + 1 < __argc) x = _wtoi(__wargv[++i]);
        else if (!wcscmp(a, L"--y") && i + 1 < __argc) y = _wtoi(__wargv[++i]);
        else if (!wcscmp(a, L"--seconds") && i + 1 < __argc) seconds = _wtoi(__wargv[++i]);
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    glass::Init();
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GlassDemo";
    RegisterClassExW(&wc);
    RECT r{0, 0, 620, 396};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    HWND w = CreateWindowExW(WS_EX_TOPMOST, L"GlassDemo", L"VR EyeCapture -- glass demo", style, x, y, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, inst, nullptr);
    glass::ApplyWindow(w);
    Build(w);
    ShowWindow(w, SW_SHOW);
    if (seconds > 0) SetTimer(w, 1, (UINT)seconds * 1000, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
