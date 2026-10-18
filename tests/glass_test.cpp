// glass_test -- the glass controls (tray/glass.h) answer the classic messages the tray's windows and tests send: BUTTON
// (BM_*, BN_CLICKED), COMBOBOX (CB_*, CBN_SELCHANGE only for user picks), trackbar (TBM_*), progress (PBM_*), SysLink (NM_CLICK
// with the URL), the own message dialog (class GlassMessage, text in id 0xFFFF, Yes / No buttons, Esc), the theme switch at
// run time. CPU only, a hidden window. Exit code 0 = pass.
#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <string>
#include "glass.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static int g_clicked = 0, g_selchange = 0, g_hscroll = 0;
static std::wstring g_url;

static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) ++g_clicked;
        if (HIWORD(wp) == CBN_SELCHANGE) ++g_selchange;
        return 0;
    case WM_HSCROLL:
        ++g_hscroll;
        return 0;
    case WM_NOTIFY: {
        const NMLINK* nl = (const NMLINK*)lp;
        if (nl->hdr.code == NM_CLICK || nl->hdr.code == NM_RETURN) g_url = nl->item.szUrl;
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND Make(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 10, 10, 300, 26, p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

struct BoxAnswer { std::wstring text; bool found = false; int clickId; };
static DWORD WINAPI Answer(LPVOID p) {
    BoxAnswer* a = (BoxAnswer*)p;
    for (int i = 0; i < 100; ++i) {
        HWND box = FindWindowW(L"GlassMessage", L"glass_test");
        if (box) {
            Sleep(100);
            wchar_t t[512] = L"";
            GetDlgItemTextW(box, 0xFFFF, t, 512);
            a->text = t;
            HWND b = GetDlgItem(box, a->clickId);
            a->found = b != nullptr;
            if (b) PostMessageW(box, WM_COMMAND, MAKEWPARAM(a->clickId, BN_CLICKED), (LPARAM)b);
            else PostMessageW(box, WM_KEYDOWN, VK_ESCAPE, 0);
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

int wmain() {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    glass::Init();
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GlassTestParent";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"GlassTestParent", L"glass_test parent", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);

    printf("button / check\n");
    HWND chk = Make(w, glass::kButton, L"Check", BS_AUTOCHECKBOX | WS_TABSTOP, 1);
    Check(SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "new check box is unchecked");
    SendMessageW(chk, BM_CLICK, 0, 0);
    Check(SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_CHECKED && g_clicked == 1, "BM_CLICK toggles it and sends BN_CLICKED");
    SendMessageW(chk, BM_SETCHECK, BST_UNCHECKED, 0);
    Check(SendMessageW(chk, BM_GETCHECK, 0, 0) == BST_UNCHECKED && g_clicked == 1, "BM_SETCHECK sets it without BN_CLICKED");
    HWND btn = Make(w, glass::kButton, L"Push", BS_PUSHBUTTON | WS_TABSTOP, 2);
    SendMessageW(btn, BM_CLICK, 0, 0);
    Check(g_clicked == 2 && SendMessageW(btn, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "push button: BN_CLICKED, no check state");
    wchar_t t[64] = L"";
    SetWindowTextW(btn, L"Renamed");
    GetWindowTextW(btn, t, 64);
    Check(wcscmp(t, L"Renamed") == 0, "WM_SETTEXT / WM_GETTEXT");
    HWND hidden = CreateWindowExW(0, glass::kButton, L"", WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, w, (HMENU)3, wc.hInstance, nullptr);
    SendMessageW(hidden, BM_SETCHECK, BST_CHECKED, 0);
    Check(SendMessageW(hidden, BM_GETCHECK, 0, 0) == BST_CHECKED, "a hidden 0x0 check box keeps its state");

    printf("combo\n");
    HWND cb = Make(w, glass::kCombo, L"", WS_TABSTOP | WS_VSCROLL, 4);
    Check((GetWindowLongW(cb, GWL_STYLE) & WS_VSCROLL) == 0, "no window scroll bar (WS_VSCROLL removed)");
    for (const wchar_t* it : {L"Alpha", L"Beta", L"Gamma"}) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)it);
    Check(SendMessageW(cb, CB_GETCOUNT, 0, 0) == 3 && SendMessageW(cb, CB_GETCURSEL, 0, 0) == CB_ERR, "3 items, nothing selected");
    SendMessageW(cb, CB_SETCURSEL, 1, 0);
    wchar_t item[64] = L"";
    SendMessageW(cb, CB_GETLBTEXT, 1, (LPARAM)item);
    GetWindowTextW(cb, t, 64);
    Check(SendMessageW(cb, CB_GETCURSEL, 0, 0) == 1 && wcscmp(item, L"Beta") == 0 && wcscmp(t, L"Beta") == 0 && g_selchange == 0,
          "CB_SETCURSEL selects (no CBN_SELCHANGE); CB_GETLBTEXT / WM_GETTEXT give the item");
    Check(SendMessageW(cb, CB_GETLBTEXTLEN, 2, 0) == 5 && SendMessageW(cb, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)L"gamma") == 2,
          "CB_GETLBTEXTLEN, CB_FINDSTRINGEXACT (case-insensitive)");
    SetFocus(cb);
    SendMessageW(cb, WM_KEYDOWN, VK_DOWN, 0);
    Check(SendMessageW(cb, CB_GETCURSEL, 0, 0) == 2 && g_selchange == 1, "Down arrow picks the next item with CBN_SELCHANGE");
    SendMessageW(cb, CB_SHOWDROPDOWN, TRUE, 0);
    Check(SendMessageW(cb, CB_GETDROPPEDSTATE, 0, 0) == TRUE, "CB_SHOWDROPDOWN opens the list");
    SendMessageW(cb, WM_KEYDOWN, VK_UP, 0);
    SendMessageW(cb, WM_KEYDOWN, VK_RETURN, 0);
    Check(SendMessageW(cb, CB_GETDROPPEDSTATE, 0, 0) == FALSE && SendMessageW(cb, CB_GETCURSEL, 0, 0) == 1 && g_selchange == 2,
          "in the open list Up + Enter picks Beta and closes it");
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    Check(SendMessageW(cb, CB_GETCOUNT, 0, 0) == 0 && SendMessageW(cb, CB_GETCURSEL, 0, 0) == CB_ERR, "CB_RESETCONTENT");

    printf("slider / progress\n");
    HWND sl = Make(w, glass::kSlider, L"", WS_TABSTOP, 5);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELONG(0, 20));
    SendMessageW(sl, TBM_SETPOS, TRUE, 12);
    Check(SendMessageW(sl, TBM_GETPOS, 0, 0) == 12 && g_hscroll == 0, "TBM_SETRANGE / SETPOS / GETPOS, no WM_HSCROLL");
    SendMessageW(sl, TBM_SETPOS, TRUE, 99);
    Check(SendMessageW(sl, TBM_GETPOS, 0, 0) == 20, "TBM_SETPOS clamps to the range");
    SendMessageW(sl, TBM_SETLINESIZE, 0, 1);
    SendMessageW(sl, WM_KEYDOWN, VK_LEFT, 0);
    Check(SendMessageW(sl, TBM_GETPOS, 0, 0) == 19 && g_hscroll >= 1, "Left arrow moves one line with WM_HSCROLL");
    HWND pb = Make(w, glass::kProgress, L"", 0, 6);
    SendMessageW(pb, PBM_SETRANGE32, 0, 100);
    SendMessageW(pb, PBM_SETPOS, 40, 0);
    Check(SendMessageW(pb, PBM_GETPOS, 0, 0) == 40 && SendMessageW(pb, PBM_GETSTATE, 0, 0) == PBST_NORMAL, "PBM_SETPOS / GETPOS, normal state");
    SendMessageW(pb, PBM_SETSTATE, PBST_PAUSED, 0);
    Check(SendMessageW(pb, PBM_GETSTATE, 0, 0) == PBST_PAUSED, "PBM_SETSTATE paused");

    printf("link / static\n");
    HWND lk = Make(w, glass::kLink, L"see <a href=\"https://example.org/a\">A</a> and <a href=\"https://example.org/b\">B</a>", WS_TABSTOP, 7);
    SetFocus(lk);
    SendMessageW(lk, WM_KEYDOWN, VK_RETURN, 0);
    Check(g_url == L"https://example.org/a", "Enter: NM_RETURN with the first link's URL");
    HWND st = Make(w, glass::kStatic, L"label", SS_LEFT, 8);
    glass::SetDim(st, true);
    GetWindowTextW(st, t, 64);
    Check(wcscmp(t, L"label") == 0, "static text");

    printf("theme\n");
    glass::ForceDark(1);
    Check(glass::Current().dark, "ForceDark(1) -> dark");
    glass::ForceDark(0);
    Check(!glass::Current().dark && glass::OnSettingChange(w) == false, "ForceDark(0) -> light; nothing changed since");
    const COLORREF a = glass::Current().accent;
    Check(a != glass::Current().accentText, "the accent text colour differs from the accent");

    printf("message dialog\n");
    {
        BoxAnswer ans;
        ans.clickId = IDNO;
        HANDLE th = CreateThread(nullptr, 0, Answer, &ans, 0, nullptr);
        const int r = glass::MessageBox(w, L"Question text?", L"glass_test", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
        Check(ans.found && ans.text == L"Question text?" && r == IDNO, "Yes / No dialog: text in id 0xFFFF, the No button answers IDNO");
    }
    {
        BoxAnswer ans;
        ans.clickId = 0;   // no button: Escape
        HANDLE th = CreateThread(nullptr, 0, Answer, &ans, 0, nullptr);
        const int r = glass::MessageBox(w, L"Information.", L"glass_test", MB_OK | MB_ICONINFORMATION);
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
        Check(r == IDOK, "OK dialog: Escape answers IDOK");
    }
    Check(IsWindowEnabled(w) != FALSE, "the owner is enabled again after the dialog");

    DestroyWindow(w);
    printf(g_fail ? "glass_test: FAILED (%d)\n" : "glass_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
