// glass -- see glass.h.
#include "glass.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#pragma warning(push)
#pragma warning(disable : 4458)   // gdiplus headers: declaration hides class member
#include <objidl.h>
#include <gdiplus.h>
#pragma warning(pop)
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace glass {

const wchar_t* const kButton = L"GlassButton";
const wchar_t* const kCombo = L"GlassCombo";
const wchar_t* const kStatic = L"GlassStatic";
const wchar_t* const kSlider = L"GlassSlider";
const wchar_t* const kLink = L"GlassLink";
const wchar_t* const kProgress = L"GlassProgress";
const wchar_t* const kMessageClass = L"GlassMessage";

namespace {

const wchar_t* const kComboList = L"GlassComboList";
Theme g_theme;
int g_force = -1, g_forceMica = -1;
bool g_inited = false;
ULONG_PTR g_gdipToken = 0;

// ------------------------------------------------------------------------------------------------ theme

DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    return RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS ? v : def;
}

bool WindowsBuildAtLeast(DWORD build) {
    typedef LONG(WINAPI * PFN)(OSVERSIONINFOW*);
    static DWORD s_build = 0;
    if (!s_build) {
        OSVERSIONINFOW vi{sizeof(vi)};
        PFN f = (PFN)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (f && f(&vi) == 0) s_build = vi.dwBuildNumber;
        else s_build = 1;
    }
    return s_build >= build;
}

double Luma(COLORREF c) { return (0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c)) / 255.0; }

Theme ReadTheme() {
    Theme t;
    const wchar_t* pers = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    t.dark = g_force >= 0 ? g_force == 1 : RegDword(HKEY_CURRENT_USER, pers, L"AppsUseLightTheme", 1) == 0;
    const bool transparency = RegDword(HKEY_CURRENT_USER, pers, L"EnableTransparency", 1) != 0;
    t.mica = g_forceMica >= 0 ? (g_forceMica == 1 && WindowsBuildAtLeast(22621)) : transparency && WindowsBuildAtLeast(22621);
    // Accent palette: 8 RGBA entries, light3 light2 light1 base dark1 dark2 dark3 (+1). Windows 11 uses dark1 on light
    // backgrounds and light2 on dark ones for filled controls.
    BYTE pal[32];
    DWORD sz = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, pal, &sz) == ERROR_SUCCESS && sz >= 28) {
        const int i = t.dark ? 1 : 4;
        t.accent = RGB(pal[i * 4], pal[i * 4 + 1], pal[i * 4 + 2]);
    } else {
        DWORD c = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&c, &opaque))) t.accent = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
    t.accentText = Luma(t.accent) > 0.6 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    if (t.dark) {
        t.text = RGB(255, 255, 255);
        t.textDim = RGB(200, 200, 200);
        t.textDisabled = RGB(120, 120, 120);
        t.window = RGB(32, 32, 32);
    } else {
        t.text = RGB(27, 27, 27);
        t.textDim = RGB(96, 96, 96);
        t.textDisabled = RGB(160, 160, 160);
        t.window = RGB(243, 243, 243);
    }
    return t;
}

// ------------------------------------------------------------------------------------------------ colours (ARGB)

using Gdiplus::Color;
Color Argb(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }
Color White(BYTE a) { return Color(a, 255, 255, 255); }
Color Black(BYTE a) { return Color(a, 0, 0, 0); }

// Windows 11 (WinUI) fill / stroke colours, with Mica (translucent) or solid equivalents.
struct Palette {
    Color card, cardStroke;
    Color control, controlHover, controlPressed, controlDisabled, controlStroke, controlStrokeBottom;
    Color field, fieldFocus;
    Color popup, popupStroke, itemHover, itemSelected;
    Color track;
};
Palette Pal() {
    const Theme& t = g_theme;
    Palette p;
    if (t.dark) {
        p.card = t.mica ? White(0x0D) : Argb(RGB(43, 43, 43));
        p.cardStroke = t.mica ? Black(0x19) : Argb(RGB(29, 29, 29));
        p.control = White(0x0F); p.controlHover = White(0x15); p.controlPressed = White(0x08); p.controlDisabled = White(0x0B);
        p.controlStroke = White(0x12); p.controlStrokeBottom = White(0x0A);
        p.field = White(0x0F); p.fieldFocus = Black(0x4D);
        p.popup = Argb(RGB(44, 44, 44)); p.popupStroke = Black(0x33); p.itemHover = White(0x0F); p.itemSelected = White(0x0B);
        p.track = White(0x8B);
    } else {
        p.card = t.mica ? White(0xB3) : Argb(RGB(251, 251, 251));
        p.cardStroke = Black(0x0F);
        p.control = White(0xB3); p.controlHover = Color(0x80, 249, 249, 249); p.controlPressed = Color(0x4D, 249, 249, 249);
        p.controlDisabled = Color(0x4D, 249, 249, 249);
        p.controlStroke = Black(0x0F); p.controlStrokeBottom = Black(0x29);
        p.field = White(0xB3); p.fieldFocus = White(0xFF);
        p.popup = Argb(RGB(249, 249, 249)); p.popupStroke = Black(0x1F); p.itemHover = Black(0x09); p.itemSelected = Black(0x06);
        p.track = Black(0x72);
    }
    return p;
}

// ------------------------------------------------------------------------------------------------ painting

void RoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    const float d = r * 2;
    if (r <= 0.f) { path.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}
void FillRound(Gdiplus::Graphics& g, const Color& c, float x, float y, float w, float h, float r) {
    Gdiplus::GraphicsPath p;
    RoundRect(p, x, y, w, h, r);
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &p);
}
void StrokeRound(Gdiplus::Graphics& g, const Color& c, float x, float y, float w, float h, float r, float width = 1.f) {
    Gdiplus::GraphicsPath p;
    RoundRect(p, x + width / 2, y + width / 2, w - width, h - width, r);
    Gdiplus::Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

// One paint of a glass control: a premultiplied 32-bit buffer (alpha 0 = see-through) + GDI+ on it + composited text.
class Canvas {
public:
    Canvas(HWND h, HDC target, const RECT& rc) : hwnd_(h), rc_(rc) {
        BP_PAINTPARAMS pp{sizeof(pp)};
        pp.dwFlags = BPPF_ERASE;
        pb_ = BeginBufferedPaint(target, &rc_, BPBF_TOPDOWNDIB, &pp, &dc_);
        if (!pb_) return;
        RGBQUAD* bits = nullptr;
        int stride = 0;
        if (FAILED(GetBufferedPaintBits(pb_, &bits, &stride)) || !bits) return;
        bmp_ = new Gdiplus::Bitmap(rc_.right - rc_.left, rc_.bottom - rc_.top, stride * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        g_ = new Gdiplus::Graphics(bmp_);
        g_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g_->TranslateTransform((float)-rc_.left, (float)-rc_.top);
    }
    ~Canvas() {
        delete g_;
        delete bmp_;
        if (theme_) CloseThemeData(theme_);
        if (pb_) EndBufferedPaint(pb_, TRUE);
    }
    bool Ok() const { return g_ != nullptr; }
    Gdiplus::Graphics& G() { return *g_; }
    // Text in the window's font (GDI metrics), composited with alpha onto what is already drawn.
    void Text(const std::wstring& t, RECT r, COLORREF c, DWORD flags, HFONT font) {
        if (t.empty()) return;
        g_->Flush(Gdiplus::FlushIntentionSync);
        if (!theme_) theme_ = OpenThemeData(hwnd_, L"WINDOW");
        HGDIOBJ old = font ? SelectObject(dc_, font) : nullptr;
        DTTOPTS o{sizeof(o)};
        o.dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR;
        o.crText = c;
        DrawThemeTextEx(theme_, dc_, 0, 0, t.c_str(), (int)t.size(), flags | DT_NOPREFIX, &r, &o);
        if (old) SelectObject(dc_, old);
    }
    HDC Dc() const { return dc_; }

private:
    HWND hwnd_;
    RECT rc_;
    HDC dc_ = nullptr;
    HPAINTBUFFER pb_ = nullptr;
    Gdiplus::Bitmap* bmp_ = nullptr;
    Gdiplus::Graphics* g_ = nullptr;
    HTHEME theme_ = nullptr;
};

HFONT FontOf(HWND h) {
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}
std::wstring TextOf(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}
bool FocusCues(HWND h) { return !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS); }

bool IsGroup(HWND h) {
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    return wcscmp(cls, kButton) == 0 && (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX;
}

struct RectF_ { float x, y, w, h; };
bool IsPanel(HWND h);
Color PanelFill() { return g_theme.dark ? White(0x08) : White(0x80); }

// What lies under a control: the card of the group box that contains it (painted first), else nothing (the Mica /
// the solid window background).
void PaintUnder(HWND h, Gdiplus::Graphics& g, const RECT& rc) {
    const Theme& t = g_theme;
    if (!t.mica) g.Clear(Argb(t.window));
    HWND parent = GetParent(h);
    RECT me;
    GetWindowRect(h, &me);
    for (HWND s = GetWindow(parent, GW_CHILD); s; s = GetWindow(s, GW_HWNDNEXT)) {
        if (s == h || !IsWindowVisible(s)) continue;
        const bool group = IsGroup(s), panel = !group && IsPanel(s);
        if (!group && !panel) continue;
        RECT gr;
        GetWindowRect(s, &gr);
        if (me.left >= gr.left && me.top >= gr.top + (group ? 8 : 0) && me.right <= gr.right && me.bottom <= gr.bottom) {
            const RectF_ r{(float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top)};
            Gdiplus::SolidBrush b(group ? Pal().card : PanelFill());
            g.FillRectangle(&b, r.x, r.y, r.w, r.h);
            return;
        }
    }
}

// ------------------------------------------------------------------------------------------------ per-control state

struct State {
    bool hover = false, pressed = false, tracking = false;
    LRESULT check = BST_UNCHECKED;
    COLORREF color = CLR_INVALID;
    bool dim = false, panel = false, odTransparent = false;
    // combo
    std::vector<std::wstring> items;
    std::vector<LPARAM> data;
    int sel = -1;
    int droppedW = 0;
    HWND list = nullptr;
    // slider
    int lo = 0, hi = 100, pos = 0, page = 10, line = 1;   // slider; progress: page = the PBST_* state
    bool drag = false;
};
State* St(HWND h) { return (State*)GetWindowLongPtrW(h, 0); }
bool IsPanel(HWND h) {
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    if (wcscmp(cls, kStatic) != 0) return false;
    const State* s = St(h);
    return s && s->panel;
}

void TrackLeave(HWND h, State* s) {
    if (s->tracking) return;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
    if (TrackMouseEvent(&tme)) s->tracking = true;
}

LRESULT CommonMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp, bool* handled) {
    *handled = true;
    State* s = St(h);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, 0, (LONG_PTR) new State);
        // a classic combo is made with WS_VSCROLL for its list: a glass control never has window scroll bars
        SetWindowLongW(h, GWL_STYLE, GetWindowLongW(h, GWL_STYLE) & ~(WS_VSCROLL | WS_HSCROLL));
        *handled = false;
        return 0;
    case WM_NCDESTROY:
        delete s;
        SetWindowLongPtrW(h, 0, 0);
        *handled = false;
        return 0;
    case WM_SETFONT:
        SetPropW(h, L"GlassFont", (HANDLE)wp);
        if (LOWORD(lp)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETFONT:
        return (LRESULT)GetPropW(h, L"GlassFont");
    case WM_SETTEXT: {
        const LRESULT r = DefWindowProcW(h, msg, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
        InvalidateRect(h, nullptr, FALSE);
        *handled = false;
        return 0;
    case WM_MOUSELEAVE:
        if (s) { s->hover = false; s->tracking = false; s->pressed = s->pressed && GetCapture() == h; }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        *handled = false;
        return 0;
    }
}

void Notify(HWND h, WORD code) {
    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), code), (LPARAM)h);
}

void FocusRing(Gdiplus::Graphics& g, const RECT& rc, float r) {
    StrokeRound(g, Argb(g_theme.text), (float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), r + 2, 2.f);
}

// ------------------------------------------------------------------------------------------------ button / check / group

void PaintButton(HWND h, HDC dc) {
    State* s = St(h);
    RECT rc;
    GetClientRect(h, &rc);
    Canvas cv(h, dc, rc);
    if (!cv.Ok()) return;
    auto& g = cv.G();
    PaintUnder(h, g, rc);
    const Theme& t = g_theme;
    const Palette p = Pal();
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    const LONG type = style & BS_TYPEMASK;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const bool focus = GetFocus() == h && FocusCues(h);
    const HFONT font = FontOf(h);
    const std::wstring text = TextOf(h);
    const float W = (float)rc.right, H = (float)rc.bottom;
    if (type == BS_GROUPBOX) {   // a card: rounded panel, the title inside its top-left
        FillRound(g, p.card, 0.5f, 7.5f, W - 1, H - 8, 6);
        StrokeRound(g, p.cardStroke, 0, 7, W, H - 7, 6);
        HDC mdc = cv.Dc();
        HGDIOBJ old = SelectObject(mdc, font);
        SIZE ts{};
        GetTextExtentPoint32W(mdc, text.c_str(), (int)text.size(), &ts);
        SelectObject(mdc, old);
        // the title sits on the card's top edge like a classic group box caption, on a pill of the window colour
        if (!text.empty()) {
            const float tx = 10, tw = (float)ts.cx + 8;
            if (!t.mica) {
                Gdiplus::SolidBrush b(Argb(t.window));
                g.FillRectangle(&b, tx - 4, 0.f, tw, (float)ts.cy);
            } else {
                g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                Gdiplus::SolidBrush clear(Color(0, 0, 0, 0));
                g.FillRectangle(&clear, tx - 4, 0.f, tw, 14.f);
                g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
            }
            RECT tr{(LONG)tx, 0, (LONG)(tx + ts.cx + 2), ts.cy};
            cv.Text(text, tr, enabled ? t.text : t.textDisabled, DT_LEFT | DT_TOP | DT_SINGLELINE, font);
        }
        return;
    }
    if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX || type == BS_AUTO3STATE || type == BS_3STATE) {
        const float box = 16, by = (H - box) / 2;   // text at kCheckTextX
        const bool on = s->check == BST_CHECKED;
        if (on) {
            Color fill = enabled ? Argb(t.accent) : (t.dark ? White(0x28) : Black(0x37));
            if (enabled && s->pressed) fill = Argb(t.accent, 0xCC);
            else if (enabled && s->hover) fill = Argb(t.accent, 0xE6);
            FillRound(g, fill, 0.5f, by, box, box, 4);
            Gdiplus::Pen pen(Argb(enabled ? t.accentText : t.window), 1.6f);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            Gdiplus::PointF pts[3] = {{4.f, by + 8.5f}, {7.f, by + 11.5f}, {12.6f, by + 5.f}};
            g.DrawLines(&pen, pts, 3);
        } else {
            FillRound(g, s->pressed ? p.controlPressed : s->hover ? p.controlHover : (t.dark ? Black(0x1A) : Black(0x05)), 0.5f, by, box, box, 4);
            StrokeRound(g, enabled ? (t.dark ? White(0x8B) : Black(0x72)) : (t.dark ? White(0x28) : Black(0x37)), 0.5f, by, box, box, 4);
        }
        RECT tr{kCheckTextX, 0, rc.right, rc.bottom};
        cv.Text(text, tr, enabled ? t.text : t.textDisabled, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, font);
        if (focus) {
            RECT fr{-1, (LONG)by - 2, (LONG)box + 2, (LONG)(by + box) + 2};
            FocusRing(g, fr, 4);
        }
        return;
    }
    // push button
    const bool def = type == BS_DEFPUSHBUTTON;
    Color fill = p.control;
    COLORREF tc = enabled ? t.text : t.textDisabled;
    if (!enabled) fill = p.controlDisabled;
    else if (s->pressed) fill = p.controlPressed;
    else if (s->hover) fill = p.controlHover;
    if (def && enabled) {
        fill = Argb(t.accent, s->pressed ? 0xCC : s->hover ? 0xE6 : 0xFF);
        tc = t.accentText;
    }
    FillRound(g, fill, 1, 1, W - 2, H - 2, 4);
    if (!(def && enabled)) {
        StrokeRound(g, p.controlStroke, 1, 1, W - 2, H - 2, 4);
        Gdiplus::Pen pen(p.controlStrokeBottom, 1.f);
        g.DrawLine(&pen, 4.f, H - 1.5f, W - 4.f, H - 1.5f);
    }
    cv.Text(text, rc, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, font);
    if (focus) { RECT fr{0, 0, rc.right, rc.bottom}; FocusRing(g, fr, 3); }
}

void ToggleCheck(HWND h) {
    State* s = St(h);
    const LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    if (type == BS_AUTOCHECKBOX) s->check = s->check == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED;
    else if (type == BS_AUTO3STATE) s->check = (s->check + 1) % 3;
    InvalidateRect(h, nullptr, FALSE);
}

void Click(HWND h) {
    const LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    if (type == BS_GROUPBOX) return;
    ToggleCheck(h);
    Notify(h, BN_CLICKED);
}

LRESULT CALLBACK ButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    const LRESULT r = CommonMessage(h, msg, wp, lp, &handled);
    if (handled) return r;
    State* s = St(h);
    const bool group = s && (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintButton(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        if (group) return HTTRANSPARENT;
        break;
    case WM_GETDLGCODE: {
        const LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
        if (type == BS_DEFPUSHBUTTON) return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
        if (type == BS_PUSHBUTTON) return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
        if (type == BS_GROUPBOX) return DLGC_STATIC;
        return DLGC_BUTTON;
    }
    case WM_MOUSEMOVE:
        if (!s->hover) { s->hover = true; InvalidateRect(h, nullptr, FALSE); }
        TrackLeave(h, s);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (group) return 0;
        s->pressed = true;
        SetCapture(h);
        if (GetFocus() != h && (GetWindowLongW(h, GWL_STYLE) & WS_TABSTOP)) SetFocus(h);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (s->pressed) {
            s->pressed = false;
            ReleaseCapture();
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT rc;
            GetClientRect(h, &rc);
            InvalidateRect(h, nullptr, FALSE);
            if (PtInRect(&rc, pt)) Click(h);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (s->pressed) { s->pressed = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE) { s->pressed = true; InvalidateRect(h, nullptr, FALSE); return 0; }
        break;
    case WM_KEYUP:
        if (wp == VK_SPACE && s->pressed) { s->pressed = false; InvalidateRect(h, nullptr, FALSE); Click(h); return 0; }
        break;
    case BM_GETCHECK:
        return s->check;
    case BM_SETCHECK:
        if (s->check != (LRESULT)wp) { s->check = (LRESULT)wp; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case BM_GETSTATE:
        return s->check | (s->pressed ? BST_PUSHED : 0) | (GetFocus() == h ? BST_FOCUS : 0);
    case BM_CLICK:
        Click(h);
        return 0;
    case BM_SETSTYLE: {
        LONG st = GetWindowLongW(h, GWL_STYLE);
        st = (st & ~BS_TYPEMASK) | ((LONG)wp & BS_TYPEMASK);
        SetWindowLongW(h, GWL_STYLE, st);
        if (LOWORD(lp)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ static

// SS_OWNERDRAW: the parent paints through WM_DRAWITEM into a 32-bit memory bitmap with plain GDI. Opaque (the default):
// every pixel it paints is made opaque. Transparent (SetOwnerDrawTransparent): it paints twice, on black and on white, with
// DRAWITEMSTRUCT.itemData = the background colour it must fill with (black / white); the two results give each pixel's
// alpha, so anti-aliased edges blend into the card / Mica below.
void PaintOwnerDraw(HWND h, Canvas& cv, const RECT& rc, bool transparent) {
    const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    if (w <= 0 || hh <= 0) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const int passes = transparent ? 2 : 1;
    BYTE* bits[2] = {nullptr, nullptr};
    HBITMAP bmp[2] = {nullptr, nullptr};
    HDC mdc = CreateCompatibleDC(nullptr);
    for (int pass = 0; pass < passes; ++pass) {
        bmp[pass] = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, (void**)&bits[pass], nullptr, 0);
        if (!bmp[pass]) break;
        HGDIOBJ old = SelectObject(mdc, bmp[pass]);
        const COLORREF bg = pass == 0 ? RGB(0, 0, 0) : RGB(255, 255, 255);
        HBRUSH b = CreateSolidBrush(bg);
        RECT all{0, 0, w, hh};
        FillRect(mdc, &all, b);
        DeleteObject(b);
        DRAWITEMSTRUCT di{};
        di.CtlType = ODT_STATIC;
        di.CtlID = (UINT)GetDlgCtrlID(h);
        di.itemAction = ODA_DRAWENTIRE;
        di.hwndItem = h;
        di.hDC = mdc;
        di.rcItem = all;
        di.itemData = (ULONG_PTR)bg;
        SendMessageW(GetParent(h), WM_DRAWITEM, (WPARAM)di.CtlID, (LPARAM)&di);
        GdiFlush();
        SelectObject(mdc, old);
    }
    DeleteDC(mdc);
    if (bits[0] && (!transparent || bits[1])) {
        const size_t n = (size_t)w * hh;
        for (size_t i = 0; i < n; ++i) {
            BYTE* p = bits[0] + i * 4;
            if (!transparent) { p[3] = 255; continue; }
            const BYTE* q = bits[1] + i * 4;
            // on black the pixel is c*a, on white c*a + 255*(1-a): the difference is 255*(1-a)
            int a = 255 - ((int)q[1] - (int)p[1]);
            a = a < 0 ? 0 : a > 255 ? 255 : a;
            p[3] = (BYTE)a;
            for (int k = 0; k < 3; ++k) if (p[k] > a) p[k] = (BYTE)a;   // keep it valid premultiplied data
        }
        Gdiplus::Bitmap img(w, hh, w * 4, PixelFormat32bppPARGB, bits[0]);
        cv.G().DrawImage(&img, rc.left, rc.top, w, hh);
    }
    for (HBITMAP b : bmp) if (b) DeleteObject(b);
}

LRESULT CALLBACK StaticProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    const LRESULT r = CommonMessage(h, msg, wp, lp, &handled);
    if (handled) return r;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        {
            Canvas cv(h, dc, rc);
            if (cv.Ok()) {
                State* s = St(h);
                const Theme& t = g_theme;
                const LONG st = GetWindowLongW(h, GWL_STYLE);
                PaintUnder(h, cv.G(), rc);
                if (s->panel) {   // a layer strip (the footer): a lighter / darker fill and a divider at its top
                    const Palette p = Pal();
                    Gdiplus::SolidBrush b(PanelFill());
                    cv.G().FillRectangle(&b, 0.f, 0.f, (float)rc.right, (float)rc.bottom);
                    Gdiplus::Pen pen(p.cardStroke, 1.f);
                    cv.G().DrawLine(&pen, 0.f, 0.5f, (float)rc.right, 0.5f);
                } else if ((st & SS_TYPEMASK) == SS_OWNERDRAW) {
                    PaintOwnerDraw(h, cv, rc, s->odTransparent);
                } else {
                    COLORREF c = s->color != CLR_INVALID ? s->color : s->dim ? t.textDim : t.text;
                    if (!IsWindowEnabled(h)) c = t.textDisabled;
                    DWORD f = DT_WORDBREAK;
                    const LONG align = st & SS_TYPEMASK;
                    if (align == SS_CENTER) f |= DT_CENTER;
                    else if (align == SS_RIGHT) f |= DT_RIGHT;
                    if ((st & SS_ELLIPSISMASK) == SS_ENDELLIPSIS) f = (f & ~DT_WORDBREAK) | DT_SINGLELINE | DT_END_ELLIPSIS;
                    if ((st & SS_ELLIPSISMASK) == SS_PATHELLIPSIS) f = (f & ~DT_WORDBREAK) | DT_SINGLELINE | DT_PATH_ELLIPSIS;
                    if (st & SS_CENTERIMAGE) f = (f & ~DT_WORDBREAK) | DT_SINGLELINE | DT_VCENTER;
                    cv.Text(TextOf(h), rc, c, f, FontOf(h));
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        return (GetWindowLongW(h, GWL_STYLE) & SS_NOTIFY) ? HTCLIENT : HTTRANSPARENT;
    case WM_LBUTTONDOWN:
        if (GetWindowLongW(h, GWL_STYLE) & SS_NOTIFY) Notify(h, STN_CLICKED);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ link

// Text with <a href="url">parts</a>, like SysLink: the parts in the accent colour, a click / Enter sends WM_NOTIFY NM_CLICK /
// NM_RETURN with an NMLINK (item.iLink, item.szUrl). LWS_RIGHT right-aligns the line.
struct LinkSeg { std::wstring text, url; int x0 = 0, x1 = 0; };
std::vector<LinkSeg> ParseLink(const std::wstring& s) {
    std::vector<LinkSeg> out;
    size_t p = 0;
    while (p < s.size()) {
        const size_t a = s.find(L"<a", p);
        if (a == std::wstring::npos) { out.push_back({s.substr(p), L""}); break; }
        if (a > p) out.push_back({s.substr(p, a - p), L""});
        const size_t gt = s.find(L'>', a), end = s.find(L"</a>", a);
        if (gt == std::wstring::npos || end == std::wstring::npos || end < gt) { out.push_back({s.substr(a), L""}); break; }
        std::wstring url;
        const size_t h1 = s.find(L"href=\"", a);
        if (h1 != std::wstring::npos && h1 < gt) {
            const size_t h2 = s.find(L'"', h1 + 6);
            if (h2 != std::wstring::npos && h2 < gt) url = s.substr(h1 + 6, h2 - h1 - 6);
        }
        out.push_back({s.substr(gt + 1, end - gt - 1), url.empty() ? L"#" : url});
        p = end + 4;
    }
    return out;
}
void LayoutLink(HWND h, std::vector<LinkSeg>& segs, const RECT& rc) {
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, FontOf(h));
    int total = 0;
    std::vector<int> ws;
    for (const LinkSeg& g : segs) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, g.text.c_str(), (int)g.text.size(), &sz);
        ws.push_back(sz.cx);
        total += sz.cx;
    }
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    int x = (GetWindowLongW(h, GWL_STYLE) & LWS_RIGHT) ? std::max(0, (int)rc.right - total) : 0;
    for (size_t i = 0; i < segs.size(); ++i) { segs[i].x0 = x; x += ws[i]; segs[i].x1 = x; }
}
int LinkAt(HWND h, int x) {
    RECT rc;
    GetClientRect(h, &rc);
    std::vector<LinkSeg> segs = ParseLink(TextOf(h));
    LayoutLink(h, segs, rc);
    int li = 0;
    for (const LinkSeg& g : segs) {
        if (g.url.empty()) continue;
        if (x >= g.x0 && x < g.x1) return li;
        ++li;
    }
    return -1;
}
void LinkNotify(HWND h, int link, UINT code) {
    std::vector<LinkSeg> segs = ParseLink(TextOf(h));
    int li = 0;
    for (const LinkSeg& g : segs) {
        if (g.url.empty()) continue;
        if (li == link) {
            NMLINK nl{};
            nl.hdr.hwndFrom = h;
            nl.hdr.idFrom = (UINT_PTR)GetDlgCtrlID(h);
            nl.hdr.code = code;
            nl.item.mask = LIF_ITEMINDEX | LIF_URL;
            nl.item.iLink = link;
            wcsncpy_s(nl.item.szUrl, g.url.c_str(), _TRUNCATE);
            SendMessageW(GetParent(h), WM_NOTIFY, nl.hdr.idFrom, (LPARAM)&nl);
            return;
        }
        ++li;
    }
}

LRESULT CALLBACK LinkProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    const LRESULT r = CommonMessage(h, msg, wp, lp, &handled);
    if (handled) return r;
    State* s = St(h);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        {
            Canvas cv(h, dc, rc);
            if (cv.Ok()) {
                PaintUnder(h, cv.G(), rc);
                const Theme& t = g_theme;
                std::vector<LinkSeg> segs = ParseLink(TextOf(h));
                LayoutLink(h, segs, rc);
                int li = 0;
                const HFONT font = FontOf(h);
                for (const LinkSeg& g : segs) {
                    RECT tr{g.x0, 0, g.x1 + 2, rc.bottom};
                    const bool isLink = !g.url.empty();
                    cv.Text(g.text, tr, isLink ? t.accent : t.textDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE, font);
                    if (isLink) {
                        if (li == s->sel) {   // hover: underline
                            Gdiplus::Pen pen(Argb(t.accent), 1.f);
                            const float y = rc.bottom / 2.f + 7.5f;
                            cv.G().DrawLine(&pen, (float)g.x0, y, (float)g.x1, y);
                        }
                        if (li == 0 && GetFocus() == h && FocusCues(h)) { RECT fr{g.x0 - 2, 0, g.x1 + 2, rc.bottom}; FocusRing(cv.G(), fr, 2); }
                        ++li;
                    }
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_MOUSEMOVE: {
        const int hot = LinkAt(h, GET_X_LPARAM(lp));
        if (hot != s->sel) { s->sel = hot; InvalidateRect(h, nullptr, FALSE); }
        TrackLeave(h, s);
        return 0;
    }
    case WM_MOUSELEAVE:
        s->sel = -1;
        s->tracking = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(h, &pt);
        if (LinkAt(h, pt.x) >= 0) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    case WM_LBUTTONUP: {
        const int l = LinkAt(h, GET_X_LPARAM(lp));
        if (l >= 0) LinkNotify(h, l, NM_CLICK);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_SPACE) { LinkNotify(h, 0, NM_RETURN); return 0; }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ progress

LRESULT CALLBACK ProgressProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    const LRESULT r = CommonMessage(h, msg, wp, lp, &handled);
    if (handled) return r;
    State* s = St(h);
    switch (msg) {
    case WM_CREATE:
        s->page = PBST_NORMAL;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        {
            Canvas cv(h, dc, rc);
            if (cv.Ok()) {
                auto& g = cv.G();
                PaintUnder(h, g, rc);
                const Theme& t = g_theme;
                const float cy = rc.bottom / 2.f, W = (float)rc.right;
                FillRound(g, t.dark ? White(0x8B) : Black(0x72), 0, cy - 0.5f, W, 1, 0.5f);
                const int span = std::max(1, s->hi - s->lo);
                const float fw = W * std::max(0, std::min(span, s->pos - s->lo)) / span;
                COLORREF c = t.accent;
                if (s->page == PBST_PAUSED) c = RGB(157, 93, 0);
                else if (s->page == PBST_ERROR) c = RGB(196, 43, 28);
                if (fw > 0) FillRound(g, Argb(c), 0, cy - 2, std::max(4.f, fw), 4, 2);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        return HTCLIENT;
    case PBM_SETRANGE32: s->lo = (int)wp; s->hi = (int)lp; InvalidateRect(h, nullptr, FALSE); return 0;
    case PBM_SETRANGE: s->lo = LOWORD(lp); s->hi = HIWORD(lp); InvalidateRect(h, nullptr, FALSE); return 0;
    case PBM_GETRANGE: return wp ? s->lo : s->hi;
    case PBM_SETPOS: { const int o = s->pos; s->pos = (int)wp; InvalidateRect(h, nullptr, FALSE); return o; }
    case PBM_GETPOS: return s->pos;
    case PBM_DELTAPOS: { const int o = s->pos; s->pos += (int)wp; InvalidateRect(h, nullptr, FALSE); return o; }
    case PBM_SETSTATE: { const int o = s->page; s->page = (int)wp; InvalidateRect(h, nullptr, FALSE); return o; }
    case PBM_GETSTATE: return s->page;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ combo

void PaintCombo(HWND h, HDC dc) {
    State* s = St(h);
    RECT rc;
    GetClientRect(h, &rc);
    Canvas cv(h, dc, rc);
    if (!cv.Ok()) return;
    auto& g = cv.G();
    PaintUnder(h, g, rc);
    const Theme& t = g_theme;
    const Palette p = Pal();
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const float W = (float)rc.right, H = (float)rc.bottom;
    Color fill = !enabled ? p.controlDisabled : s->pressed || s->list ? p.controlPressed : s->hover ? p.controlHover : p.control;
    FillRound(g, fill, 1, 1, W - 2, H - 2, 4);
    StrokeRound(g, p.controlStroke, 1, 1, W - 2, H - 2, 4);
    Gdiplus::Pen bottom(p.controlStrokeBottom, 1.f);
    g.DrawLine(&bottom, 4.f, H - 1.5f, W - 4.f, H - 1.5f);
    // chevron
    const float cx = W - 12, cy = H / 2;
    Gdiplus::Pen pen(Argb(enabled ? t.textDim : t.textDisabled), 1.3f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    Gdiplus::PointF pts[3] = {{cx - 4.5f, cy - 2}, {cx, cy + 2.5f}, {cx + 4.5f, cy - 2}};
    g.DrawLines(&pen, pts, 3);
    if (s->sel >= 0 && s->sel < (int)s->items.size()) {
        RECT tr{8, 0, rc.right - 23, rc.bottom};   // kComboTextPad = 8 + 23
        cv.Text(s->items[(size_t)s->sel], tr, enabled ? t.text : t.textDisabled, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, FontOf(h));
    }
    if (GetFocus() == h && FocusCues(h)) { RECT fr{0, 0, rc.right, rc.bottom}; FocusRing(g, fr, 3); }
}

int ItemH(HWND combo) {
    HDC dc = GetDC(combo);
    HGDIOBJ old = SelectObject(dc, FontOf(combo));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(combo, dc);
    return tm.tmHeight + 12;
}

void SelectItem(HWND h, int i, bool notify) {
    State* s = St(h);
    if (i < -1 || i >= (int)s->items.size() || i == s->sel) return;
    s->sel = i;
    InvalidateRect(h, nullptr, FALSE);
    if (notify) Notify(h, CBN_SELCHANGE);
}

void CloseList(HWND h, bool pick) {
    State* s = St(h);
    if (!s || !s->list) return;
    HWND l = s->list;
    const int hot = (int)GetWindowLongPtrW(l, GWLP_USERDATA);
    s->list = nullptr;
    DestroyWindow(l);
    InvalidateRect(h, nullptr, FALSE);
    if (pick && hot >= 0) SelectItem(h, hot, true);
    Notify(h, CBN_CLOSEUP);
}

struct ListData {
    HWND combo;
    int hot, top, visible, itemH;
};

void PaintList(HWND l, HDC dc) {
    ListData* d = (ListData*)GetWindowLongPtrW(l, 0);
    State* s = St(d->combo);
    RECT rc;
    GetClientRect(l, &rc);
    Canvas cv(l, dc, rc);
    if (!cv.Ok()) return;
    auto& g = cv.G();
    const Theme& t = g_theme;
    const Palette p = Pal();
    g.Clear(p.popup);
    for (int i = 0; i < d->visible && d->top + i < (int)s->items.size(); ++i) {
        const int idx = d->top + i;
        const float y = (float)(4 + i * d->itemH);
        if (idx == d->hot) FillRound(g, p.itemHover, 4, y + 1, (float)rc.right - 8, (float)d->itemH - 2, 4);
        else if (idx == s->sel) FillRound(g, p.itemSelected, 4, y + 1, (float)rc.right - 8, (float)d->itemH - 2, 4);
        if (idx == s->sel) FillRound(g, Argb(t.accent), 4, y + d->itemH / 2.f - 8, 3, 16, 1.5f);
        RECT tr{14, (LONG)y, rc.right - 8, (LONG)y + d->itemH};
        cv.Text(s->items[(size_t)idx], tr, t.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS, FontOf(d->combo));
    }
    if ((int)s->items.size() > d->visible) {   // a thin scroll indicator
        const float trackH = (float)rc.bottom - 8, th = trackH * d->visible / s->items.size();
        const float ty = 4 + (trackH - th) * d->top / std::max(1, (int)s->items.size() - d->visible);
        FillRound(g, t.dark ? White(0x8B) : Black(0x72), (float)rc.right - 5, ty, 2, th, 1);
    }
}

void SetHot(HWND l, int hot) {
    ListData* d = (ListData*)GetWindowLongPtrW(l, 0);
    State* s = St(d->combo);
    hot = std::max(0, std::min(hot, (int)s->items.size() - 1));
    d->hot = hot;
    SetWindowLongPtrW(l, GWLP_USERDATA, hot);
    if (hot < d->top) d->top = hot;
    if (hot >= d->top + d->visible) d->top = hot - d->visible + 1;
    InvalidateRect(l, nullptr, FALSE);
}

LRESULT CALLBACK ListProc(HWND l, UINT msg, WPARAM wp, LPARAM lp) {
    ListData* d = (ListData*)GetWindowLongPtrW(l, 0);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(l, 0, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(l, 0, 0);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(l, &ps);
        PaintList(l, dc);
        EndPaint(l, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        const int y = GET_Y_LPARAM(lp) - 4;
        RECT rc;
        GetClientRect(l, &rc);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&rc, pt) && y >= 0) {
            const int i = d->top + y / d->itemH;
            if (i != d->hot && i < (int)St(d->combo)->items.size()) SetHot(l, i);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        State* s = St(d->combo);
        const int maxTop = std::max(0, (int)s->items.size() - d->visible);
        d->top = std::max(0, std::min(maxTop, d->top - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3));
        InvalidateRect(l, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        RECT rc;
        GetClientRect(l, &rc);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&rc, pt)) CloseList(d->combo, true);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        RECT rc;
        GetClientRect(l, &rc);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!PtInRect(&rc, pt)) CloseList(d->combo, false);   // a click outside (we hold the capture)
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != l && d && St(d->combo) && St(d->combo)->list == l) PostMessageW(d->combo, WM_USER + 77, 0, 0);
        return 0;
    }
    return DefWindowProcW(l, msg, wp, lp);
}

void OpenList(HWND h) {
    State* s = St(h);
    if (s->list || s->items.empty()) return;
    RECT wr;
    GetWindowRect(h, &wr);
    const int ih = ItemH(h);
    const int visible = std::min((int)s->items.size(), 12);
    const int w = std::max((int)(wr.right - wr.left), s->droppedW);
    const int hgt = visible * ih + 8;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    int y = wr.bottom + 2;
    if (y + hgt > mi.rcWork.bottom) y = std::max((int)mi.rcWork.top, (int)wr.top - hgt - 2);
    int x = std::min((int)wr.left, (int)mi.rcWork.right - w);
    ListData* d = new ListData{h, s->sel, 0, visible, ih};
    HWND l = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kComboList, L"", WS_POPUP, x, y, w, hgt,
                             GetAncestor(h, GA_ROOT), nullptr, GetModuleHandleW(nullptr), d);
    if (!l) { delete d; return; }
    const DWORD corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(l, 33, &corner, sizeof(corner));
    BOOL dark = g_theme.dark;
    DwmSetWindowAttribute(l, 20, &dark, sizeof(dark));
    s->list = l;
    SetHot(l, s->sel < 0 ? 0 : s->sel);
    if (s->sel >= visible) d->top = std::min(s->sel, (int)s->items.size() - visible);
    Notify(h, CBN_DROPDOWN);
    ShowWindow(l, SW_SHOWNOACTIVATE);
    SetCapture(l);
    InvalidateRect(h, nullptr, FALSE);
}

LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    const LRESULT r = CommonMessage(h, msg, wp, lp, &handled);
    if (handled) return r;
    State* s = St(h);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintCombo(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | (s->list ? DLGC_WANTALLKEYS : 0);
    case WM_MOUSEMOVE:
        if (!s->hover) { s->hover = true; InvalidateRect(h, nullptr, FALSE); }
        TrackLeave(h, s);
        return 0;
    case WM_LBUTTONDOWN:
        if (GetFocus() != h) SetFocus(h);
        if (s->list) CloseList(h, false);
        else OpenList(h);
        return 0;
    case WM_KILLFOCUS:
        CloseList(h, false);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_USER + 77:   // the popup lost the capture (another window, Alt+Tab)
        CloseList(h, false);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_DOWN || wp == VK_UP) { if (s->list) CloseList(h, true); else OpenList(h); return 0; }
        break;
    case WM_KEYDOWN: {
        if (s->list) {
            ListData* d = (ListData*)GetWindowLongPtrW(s->list, 0);
            switch (wp) {
            case VK_UP: SetHot(s->list, d->hot - 1); return 0;
            case VK_DOWN: SetHot(s->list, d->hot + 1); return 0;
            case VK_PRIOR: SetHot(s->list, d->hot - d->visible); return 0;
            case VK_NEXT: SetHot(s->list, d->hot + d->visible); return 0;
            case VK_HOME: SetHot(s->list, 0); return 0;
            case VK_END: SetHot(s->list, (int)s->items.size() - 1); return 0;
            case VK_RETURN: case VK_SPACE: case VK_F4: CloseList(h, true); return 0;
            case VK_ESCAPE: CloseList(h, false); return 0;
            }
            return 0;
        }
        const int n = (int)s->items.size();
        switch (wp) {
        case VK_UP: case VK_LEFT: SelectItem(h, std::max(0, s->sel - 1), true); return 0;
        case VK_DOWN: case VK_RIGHT: SelectItem(h, std::min(n - 1, s->sel + 1), true); return 0;
        case VK_HOME: SelectItem(h, 0, true); return 0;
        case VK_END: SelectItem(h, n - 1, true); return 0;
        case VK_F4: case VK_SPACE: OpenList(h); return 0;
        }
        break;
    }
    case CB_ADDSTRING:
        s->items.push_back((const wchar_t*)lp);
        s->data.push_back(0);
        return (LRESULT)s->items.size() - 1;
    case CB_INSERTSTRING: {
        int i = (int)wp;
        if (i < 0 || i > (int)s->items.size()) i = (int)s->items.size();
        s->items.insert(s->items.begin() + i, (const wchar_t*)lp);
        s->data.insert(s->data.begin() + i, 0);
        if (s->sel >= i) ++s->sel;
        return i;
    }
    case CB_DELETESTRING: {
        const int i = (int)wp;
        if (i < 0 || i >= (int)s->items.size()) return CB_ERR;
        s->items.erase(s->items.begin() + i);
        s->data.erase(s->data.begin() + i);
        if (s->sel == i) s->sel = -1;
        else if (s->sel > i) --s->sel;
        InvalidateRect(h, nullptr, FALSE);
        return (LRESULT)s->items.size();
    }
    case CB_RESETCONTENT:
        s->items.clear();
        s->data.clear();
        s->sel = -1;
        InvalidateRect(h, nullptr, FALSE);
        return CB_OKAY;
    case CB_GETCOUNT:
        return (LRESULT)s->items.size();
    case CB_GETCURSEL:
        return s->sel;
    case CB_SETCURSEL:
        if ((int)wp < -1 || (int)wp >= (int)s->items.size()) { SelectItem(h, -1, false); return CB_ERR; }
        SelectItem(h, (int)wp, false);
        return s->sel;
    case CB_GETLBTEXTLEN:
        return (int)wp >= 0 && (int)wp < (int)s->items.size() ? (LRESULT)s->items[wp].size() : CB_ERR;
    case CB_GETLBTEXT:
        if ((int)wp < 0 || (int)wp >= (int)s->items.size()) return CB_ERR;
        wmemcpy((wchar_t*)lp, s->items[wp].c_str(), s->items[wp].size() + 1);
        return (LRESULT)s->items[wp].size();
    case CB_FINDSTRINGEXACT: {
        const int start = (int)wp;
        const int n = (int)s->items.size();
        for (int k = 1; k <= n; ++k) {
            const int i = (start + k) % n;
            if (_wcsicmp(s->items[(size_t)i].c_str(), (const wchar_t*)lp) == 0) return i;
        }
        return CB_ERR;
    }
    case CB_GETITEMDATA:
        return (int)wp >= 0 && (int)wp < (int)s->data.size() ? s->data[wp] : CB_ERR;
    case CB_SETITEMDATA:
        if ((int)wp < 0 || (int)wp >= (int)s->data.size()) return CB_ERR;
        s->data[wp] = lp;
        return CB_OKAY;
    case CB_SETDROPPEDWIDTH:
        s->droppedW = (int)wp;
        return s->droppedW;
    case CB_GETDROPPEDWIDTH:
        return s->droppedW;
    case CB_SHOWDROPDOWN:
        if (wp) OpenList(h); else CloseList(h, false);
        return TRUE;
    case CB_GETDROPPEDSTATE:
        return s->list != nullptr;
    case WM_GETTEXTLENGTH:
        return s->sel >= 0 ? (LRESULT)s->items[(size_t)s->sel].size() : 0;
    case WM_GETTEXT: {
        if (!wp) return 0;
        const std::wstring t = s->sel >= 0 ? s->items[(size_t)s->sel] : L"";
        const size_t n = std::min(t.size(), (size_t)wp - 1);
        wmemcpy((wchar_t*)lp, t.c_str(), n);
        ((wchar_t*)lp)[n] = 0;
        return (LRESULT)n;
    }
    case WM_DESTROY:
        CloseList(h, false);
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ slider

float ThumbX(HWND h, State* s, const RECT& rc) {
    const float l = 11, r = (float)rc.right - 11;
    const int span = std::max(1, s->hi - s->lo);
    return l + (r - l) * (s->pos - s->lo) / span;
}

void SetPos(HWND h, int p, WORD code) {
    State* s = St(h);
    p = std::max(s->lo, std::min(s->hi, p));
    if (p != s->pos) { s->pos = p; InvalidateRect(h, nullptr, FALSE); }
    if (code != 0xFFFF) SendMessageW(GetParent(h), WM_HSCROLL, MAKEWPARAM(code, code == TB_THUMBTRACK || code == TB_THUMBPOSITION ? p : 0), (LPARAM)h);
}

void PaintSlider(HWND h, HDC dc) {
    State* s = St(h);
    RECT rc;
    GetClientRect(h, &rc);
    Canvas cv(h, dc, rc);
    if (!cv.Ok()) return;
    auto& g = cv.G();
    PaintUnder(h, g, rc);
    const Theme& t = g_theme;
    const Palette p = Pal();
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const float cy = rc.bottom / 2.f, x = ThumbX(h, s, rc);
    FillRound(g, p.track, 11, cy - 2, (float)rc.right - 22, 4, 2);
    FillRound(g, enabled ? Argb(t.accent) : Argb(t.textDisabled), 11, cy - 2, x - 11, 4, 2);
    // thumb: a disc of the control colour with an accent dot that grows on hover / drag
    Gdiplus::SolidBrush outer(t.dark ? Argb(RGB(69, 69, 69)) : Argb(RGB(255, 255, 255)));
    g.FillEllipse(&outer, x - 10, cy - 10, 20.f, 20.f);
    Gdiplus::Pen ring(p.controlStroke, 1.f);
    g.DrawEllipse(&ring, x - 10, cy - 10, 20.f, 20.f);
    const float in = s->drag ? 4.f : s->hover ? 7.f : 6.f;
    Gdiplus::SolidBrush dot(enabled ? Argb(t.accent) : Argb(t.textDisabled));
    g.FillEllipse(&dot, x - in, cy - in, in * 2, in * 2);
    if (GetFocus() == h && FocusCues(h)) { RECT fr{0, 0, rc.right, rc.bottom}; FocusRing(g, fr, 3); }
}

LRESULT CALLBACK SliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    bool handled;
    const LRESULT r = CommonMessage(h, msg, wp, lp, &handled);
    if (handled) return r;
    State* s = St(h);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintSlider(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_MOUSEMOVE: {
        if (!s->hover) { s->hover = true; InvalidateRect(h, nullptr, FALSE); }
        TrackLeave(h, s);
        if (s->drag) {
            RECT rc;
            GetClientRect(h, &rc);
            const float l = 11, rr = (float)rc.right - 11;
            const float f = std::max(0.f, std::min(1.f, (GET_X_LPARAM(lp) - l) / (rr - l)));
            SetPos(h, s->lo + (int)std::lround(f * (s->hi - s->lo)), TB_THUMBTRACK);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        s->drag = true;
        SetCapture(h);
        SendMessageW(h, WM_MOUSEMOVE, wp, lp);
        return 0;
    case WM_LBUTTONUP:
        if (s->drag) {
            s->drag = false;
            ReleaseCapture();
            SetPos(h, s->pos, TB_THUMBPOSITION);
            SetPos(h, s->pos, TB_ENDTRACK);
        }
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: case VK_DOWN: SetPos(h, s->pos - s->line, TB_LINEUP); SetPos(h, s->pos, TB_ENDTRACK); return 0;
        case VK_RIGHT: case VK_UP: SetPos(h, s->pos + s->line, TB_LINEDOWN); SetPos(h, s->pos, TB_ENDTRACK); return 0;
        case VK_PRIOR: SetPos(h, s->pos - s->page, TB_PAGEUP); SetPos(h, s->pos, TB_ENDTRACK); return 0;
        case VK_NEXT: SetPos(h, s->pos + s->page, TB_PAGEDOWN); SetPos(h, s->pos, TB_ENDTRACK); return 0;
        case VK_HOME: SetPos(h, s->lo, TB_TOP); SetPos(h, s->pos, TB_ENDTRACK); return 0;
        case VK_END: SetPos(h, s->hi, TB_BOTTOM); SetPos(h, s->pos, TB_ENDTRACK); return 0;
        }
        break;
    case TBM_SETRANGE:
        s->lo = (short)LOWORD(lp);
        s->hi = (short)HIWORD(lp);
        s->pos = std::max(s->lo, std::min(s->hi, s->pos));
        if (wp) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case TBM_SETRANGEMIN: s->lo = (int)lp; if (wp) InvalidateRect(h, nullptr, FALSE); return 0;
    case TBM_SETRANGEMAX: s->hi = (int)lp; if (wp) InvalidateRect(h, nullptr, FALSE); return 0;
    case TBM_GETRANGEMIN: return s->lo;
    case TBM_GETRANGEMAX: return s->hi;
    case TBM_GETPOS: return s->pos;
    case TBM_SETPOS: SetPos(h, (int)lp, 0xFFFF); return 0;
    case TBM_SETPAGESIZE: { const int o = s->page; s->page = (int)lp; return o; }
    case TBM_SETLINESIZE: { const int o = s->line; s->line = (int)lp; return o; }
    case TBM_GETPAGESIZE: return s->page;
    case TBM_GETLINESIZE: return s->line;
    case TBM_SETTICFREQ: return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------------------------------------ edit frame

const int kEditPadX = 8, kEditPadY = 4;

LRESULT CALLBACK EditSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_NCCALCSIZE: {
        RECT* r = wp ? &((NCCALCSIZE_PARAMS*)lp)->rgrc[0] : (RECT*)lp;
        wchar_t cls[16] = L"";
        GetClassNameW(h, cls, 16);
        const bool multi = _wcsicmp(cls, L"Edit") != 0 || (GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE) != 0;   // list boxes too
        if (!multi) {   // single line: centre the text line vertically
            HDC dc = GetDC(h);
            HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
            TEXTMETRICW tm{};
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, old);
            ReleaseDC(h, dc);
            const int pad = std::max(2, ((int)(r->bottom - r->top) - (int)tm.tmHeight) / 2);
            r->left += kEditPadX; r->right -= kEditPadX; r->top += pad; r->bottom -= pad;
        } else {
            r->left += 6; r->right -= 2; r->top += 2; r->bottom -= 1;   // multi-line / list: thin (no taller than a client edge)
        }
        if (r->right < r->left) r->right = r->left;
        if (r->bottom < r->top) r->bottom = r->top;
        return 0;
    }
    case WM_NCPAINT: {
        HDC wdc = GetWindowDC(h);
        RECT wr;
        GetWindowRect(h, &wr);
        RECT rc{0, 0, wr.right - wr.left, wr.bottom - wr.top};
        RECT cr;
        GetClientRect(h, &cr);
        POINT org{0, 0};
        ClientToScreen(h, &org);
        OffsetRect(&cr, org.x - wr.left, org.y - wr.top);
        ExcludeClipRect(wdc, cr.left, cr.top, cr.right, cr.bottom);
        {
            Canvas cv(h, wdc, rc);
            if (cv.Ok()) {
                auto& g = cv.G();
                PaintUnder(h, g, rc);
                const Theme& t = g_theme;
                const Palette p = Pal();
                const bool focus = GetFocus() == h;
                wchar_t cls[16] = L"";
                GetClassNameW(h, cls, 16);
                const bool ro = _wcsicmp(cls, L"Edit") != 0 || (GetWindowLongW(h, GWL_STYLE) & ES_READONLY) != 0;
                const float W = (float)rc.right, H = (float)rc.bottom;
                Color field = Argb(t.dark ? RGB(45, 45, 45) : RGB(255, 255, 255));
                if (focus && !ro) field = Argb(t.dark ? RGB(31, 31, 31) : RGB(255, 255, 255));
                if (!IsWindowEnabled(h)) field = Argb(t.dark ? RGB(40, 40, 40) : RGB(249, 249, 249));
                FillRound(g, field, 1, 1, W - 2, H - 2, 4);
                StrokeRound(g, p.controlStroke, 1, 1, W - 2, H - 2, 4);
                if (!ro) {
                    if (focus) FillRound(g, Argb(t.accent), 2, H - 3, W - 4, 2, 1);
                    else { Gdiplus::Pen pen(t.dark ? White(0x8B) : Black(0x72), 1.f); g.DrawLine(&pen, 4.f, H - 1.5f, W - 4.f, H - 1.5f); }
                }
            }
        }
        ReleaseDC(h, wdc);
        // let the edit draw its scroll bar (multi-line) on top
        if (GetWindowLongW(h, GWL_STYLE) & WS_VSCROLL) return DefSubclassProc(h, msg, wp, lp);
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        const WINDOWPOS* wp2 = (const WINDOWPOS*)lp;
        if (!(wp2->flags & SWP_NOSIZE) && (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_LAYERED)) {
            // a layered child is one opaque surface: cut the corners off so the card / Mica shows there
            SetWindowRgn(h, CreateRoundRectRgn(0, 0, wp2->cx + 1, wp2->cy + 1, 9, 9), TRUE);
        }
        break;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, EditSubclass, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

HBRUSH g_editBrush[3] = {nullptr, nullptr, nullptr};   // normal / focused / disabled, for the current theme

// ------------------------------------------------------------------------------------------------ message dialog

struct MsgData {
    UINT flags = 0;
    int result = 0;
    bool done = false;
    int defId = IDOK;
    bool hasCancel = false;
    int iconKind = 0;   // 0 none, 1 info, 2 warning, 3 error, 4 question
    RECT iconRc{};
    int footerY = 0;
    HFONT font = nullptr, iconFont = nullptr;
};

LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MsgData* d = (MsgData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        PaintBackground(h, (HDC)wp, rc);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        {
            Canvas cv(h, dc, rc);
            if (cv.Ok() && d) {
                auto& g = cv.G();
                const Theme& t = g_theme;
                if (!t.mica) g.Clear(Argb(t.window));
                // the button strip, as in a Windows 11 content dialog
                Gdiplus::SolidBrush strip(t.dark ? White(0x08) : White(0x80));
                g.FillRectangle(&strip, 0.f, (float)d->footerY, (float)rc.right, (float)(rc.bottom - d->footerY));
                Gdiplus::Pen line(Pal().cardStroke, 1.f);
                g.DrawLine(&line, 0.f, d->footerY + 0.5f, (float)rc.right, d->footerY + 0.5f);
                if (d->iconKind) {
                    const COLORREF c = d->iconKind == 2 ? RGB(157, 93, 0) : d->iconKind == 3 ? RGB(196, 43, 28) : t.accent;
                    const RECT& ir = d->iconRc;
                    Gdiplus::SolidBrush b(Argb(d->iconKind == 2 && t.dark ? RGB(252, 225, 0) : c));
                    if (d->iconKind == 2) {   // a rounded triangle
                        Gdiplus::PointF pts[3] = {{(ir.left + ir.right) / 2.f, (float)ir.top + 1}, {(float)ir.right, (float)ir.bottom - 1}, {(float)ir.left, (float)ir.bottom - 1}};
                        g.FillPolygon(&b, pts, 3);
                    } else {
                        g.FillEllipse(&b, (float)ir.left, (float)ir.top, (float)(ir.right - ir.left), (float)(ir.bottom - ir.top));
                    }
                    const wchar_t* glyph = d->iconKind == 1 ? L"i" : d->iconKind == 2 ? L"!" : d->iconKind == 3 ? L"×" : L"?";
                    RECT gr = ir;
                    if (d->iconKind == 2) gr.top += 5;
                    cv.Text(glyph, gr, d->iconKind == 2 && t.dark ? RGB(0, 0, 0) : RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE, d->iconFont);
                }
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case DM_GETDEFID:
        return d ? MAKELRESULT(d->defId, DC_HASDEFID) : 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (!d) break;
        if (id == IDCANCEL && !d->hasCancel) {
            // Esc / the close box: like MessageBox -- OK for a lone OK button, nothing for Yes / No
            const UINT type = d->flags & MB_TYPEMASK;
            if (type == MB_OK) { d->result = IDOK; d->done = true; }
            return 0;
        }
        if (id == IDOK || id == IDCANCEL || id == IDYES || id == IDNO) { d->result = id; d->done = true; }
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
        OnSettingChange(h);
        return 0;
    case WM_CLOSE:
        SendMessageW(h, WM_COMMAND, IDCANCEL, 0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ public

void Init() {
    if (g_inited) return;
    g_inited = true;
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&g_gdipToken, &si, nullptr);
    BufferedPaintInit();
    wchar_t force[32] = L"";
    if (GetEnvironmentVariableW(L"EYECAP_TEST_GLASS", force, 32) > 0) {   // tests / screenshots only
        if (wcsstr(force, L"light")) g_force = 0;
        if (wcsstr(force, L"dark")) g_force = 1;
        if (wcsstr(force, L"solid")) g_forceMica = 0;
    }
    g_theme = ReadTheme();
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.cbWndExtra = sizeof(void*);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ButtonProc; wc.lpszClassName = kButton; RegisterClassExW(&wc);
    wc.lpfnWndProc = ComboProc; wc.lpszClassName = kCombo; RegisterClassExW(&wc);
    wc.lpfnWndProc = StaticProc; wc.lpszClassName = kStatic; RegisterClassExW(&wc);
    wc.lpfnWndProc = SliderProc; wc.lpszClassName = kSlider; RegisterClassExW(&wc);
    wc.lpfnWndProc = LinkProc; wc.lpszClassName = kLink; RegisterClassExW(&wc);
    wc.lpfnWndProc = ProgressProc; wc.lpszClassName = kProgress; RegisterClassExW(&wc);
    wc.cbWndExtra = 0;
    wc.style = 0;
    wc.lpfnWndProc = MsgProc; wc.lpszClassName = kMessageClass; RegisterClassExW(&wc);
    wc.cbWndExtra = sizeof(void*);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = ListProc; wc.lpszClassName = kComboList; RegisterClassExW(&wc);
}

const Theme& Current() { return g_theme; }

bool Reload() {
    const Theme t = ReadTheme();
    const bool changed = t.dark != g_theme.dark || t.mica != g_theme.mica || t.accent != g_theme.accent;
    g_theme = t;
    if (changed) for (HBRUSH& b : g_editBrush) if (b) { DeleteObject(b); b = nullptr; }
    return changed;
}

void ForceDark(int mode) {
    g_force = mode;
    Reload();
}

void ForceMica(int mode) {
    g_forceMica = mode;
    Reload();
}

void ApplyWindow(HWND top) {
    const Theme& t = g_theme;
    BOOL dark = t.dark;
    DwmSetWindowAttribute(top, 20, &dark, sizeof(dark));   // DWMWA_USE_IMMERSIVE_DARK_MODE
    const DWORD corner = 2;                                // DWMWCP_ROUND
    DwmSetWindowAttribute(top, 33, &corner, sizeof(corner));
    const DWORD backdrop = t.mica ? 2 : 1;                 // DWMSBT_MAINWINDOW (Mica) / DWMSBT_NONE
    DwmSetWindowAttribute(top, 38, &backdrop, sizeof(backdrop));
    MARGINS m = t.mica ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(top, &m);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void PaintBackground(HWND, HDC dc, const RECT& rc) {
    HBRUSH b = CreateSolidBrush(g_theme.mica ? RGB(0, 0, 0) : g_theme.window);   // black = alpha 0 = the Mica
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

bool OnSettingChange(HWND top) {
    if (!Reload()) return false;
    ApplyWindow(top);
    EnumChildWindows(top, [](HWND c, LPARAM) -> BOOL {
        SetWindowPos(c, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(c, nullptr, TRUE);
        return TRUE;
    }, 0);
    return true;
}

void SetDim(HWND stat, bool dim) {
    if (State* s = St(stat)) { s->dim = dim; InvalidateRect(stat, nullptr, FALSE); }
}
void SetPanel(HWND stat) {
    if (State* s = St(stat)) { s->panel = true; InvalidateRect(stat, nullptr, FALSE); }
}
void SetOwnerDrawTransparent(HWND stat) {
    if (State* s = St(stat)) { s->odTransparent = true; InvalidateRect(stat, nullptr, FALSE); }
}
void SetTextColor(HWND stat, COLORREF c) {
    if (State* s = St(stat)) { s->color = c; InvalidateRect(stat, nullptr, FALSE); }
}

void FrameEdit(HWND edit) {
    SetWindowLongW(edit, GWL_EXSTYLE, GetWindowLongW(edit, GWL_EXSTYLE) & ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
    SetWindowLongW(edit, GWL_STYLE, GetWindowLongW(edit, GWL_STYLE) & ~WS_BORDER);
    SetWindowSubclass(edit, EditSubclass, 1, 0);
    // Over the Mica (an extended DWM frame) GDI output has alpha 0 and is see-through; a layered child window gets its own
    // opaque surface (Windows 8+, needs the supportedOS manifest entry, tray/compat.manifest).
    if (g_theme.mica) {
        SetWindowLongW(edit, GWL_EXSTYLE, GetWindowLongW(edit, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
        RECT r;
        GetWindowRect(edit, &r);
        SetWindowRgn(edit, CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1, 9, 9), TRUE);
    }
    if (g_theme.dark) SetWindowTheme(edit, L"DarkMode_Explorer", nullptr);   // dark scroll bar
    SetWindowPos(edit, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

HBRUSH EditBrush(HDC dc, HWND edit) {
    const Theme& t = g_theme;
    wchar_t cls[16] = L"";
    GetClassNameW(edit, cls, 16);
    const bool isEdit = _wcsicmp(cls, L"Edit") == 0;
    const bool focus = isEdit && GetFocus() == edit && !(GetWindowLongW(edit, GWL_STYLE) & ES_READONLY);
    const bool disabled = !IsWindowEnabled(edit);
    const int i = disabled ? 2 : focus ? 1 : 0;
    const COLORREF bg = disabled ? (t.dark ? RGB(40, 40, 40) : RGB(249, 249, 249))
                      : focus ? (t.dark ? RGB(31, 31, 31) : RGB(255, 255, 255))
                              : (t.dark ? RGB(45, 45, 45) : RGB(255, 255, 255));
    if (!g_editBrush[i]) g_editBrush[i] = CreateSolidBrush(bg);
    ::SetTextColor(dc, disabled ? t.textDisabled : t.text);
    SetBkColor(dc, bg);
    return g_editBrush[i];
}

COLORREF FieldBg() {
    return g_theme.dark ? RGB(45, 45, 45) : RGB(255, 255, 255);
}

void ListRowColors(bool selected, bool focused, COLORREF* bg, COLORREF* fg) {
    const Theme& t = g_theme;
    const COLORREF base = FieldBg();
    if (selected) {   // the accent mixed into the field colour (WinUI list selection)
        const double a = focused ? 0.28 : 0.18;
        auto m = [&](int b, int f) { return (int)(b + (f - b) * a + 0.5); };
        *bg = RGB(m(GetRValue(base), GetRValue(t.accent)), m(GetGValue(base), GetGValue(t.accent)), m(GetBValue(base), GetBValue(t.accent)));
    } else {
        *bg = base;
    }
    *fg = t.text;
}

void DrawCheckBox(HDC dc, const RECT& box, bool checked, bool enabled) {
    const Theme& t = g_theme;
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float x = (float)box.left, y = (float)box.top, w = (float)(box.right - box.left);
    if (checked) {
        FillRound(g, enabled ? Argb(t.accent) : (t.dark ? Argb(RGB(80, 80, 80)) : Argb(RGB(200, 200, 200))), x, y, w, w, 4);
        Gdiplus::Pen pen(Argb(enabled ? t.accentText : t.window), 1.6f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        const float k = w / 16.f;
        Gdiplus::PointF pts[3] = {{x + 4.f * k, y + 8.5f * k}, {x + 7.f * k, y + 11.5f * k}, {x + 12.6f * k, y + 5.f * k}};
        g.DrawLines(&pen, pts, 3);
    } else {
        StrokeRound(g, enabled ? (t.dark ? Argb(RGB(160, 160, 160)) : Argb(RGB(120, 120, 120))) : (t.dark ? Argb(RGB(80, 80, 80)) : Argb(RGB(200, 200, 200))),
                    x, y, w, w, 4);
    }
}

int MessageBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags) {
    Init();
    MsgData d;
    d.flags = flags;
    struct Btn { int id; const wchar_t* text; };
    std::vector<Btn> btns;
    switch (flags & MB_TYPEMASK) {
    case MB_OKCANCEL: btns = {{IDOK, L"OK"}, {IDCANCEL, L"Cancel"}}; break;
    case MB_YESNO: btns = {{IDYES, L"&Yes"}, {IDNO, L"&No"}}; break;
    case MB_YESNOCANCEL: btns = {{IDYES, L"&Yes"}, {IDNO, L"&No"}, {IDCANCEL, L"Cancel"}}; break;
    default: btns = {{IDOK, L"OK"}}; break;
    }
    for (const Btn& b : btns) if (b.id == IDCANCEL) d.hasCancel = true;
    size_t defIdx = (flags & MB_DEFMASK) >> 8;
    if (defIdx >= btns.size()) defIdx = 0;
    d.defId = btns[defIdx].id;
    switch (flags & MB_ICONMASK) {
    case MB_ICONERROR: d.iconKind = 3; break;
    case MB_ICONQUESTION: d.iconKind = 4; break;
    case MB_ICONWARNING: d.iconKind = 2; break;
    case MB_ICONINFORMATION: d.iconKind = 1; break;
    }

    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    d.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -18;
    lf.lfWeight = FW_BOLD;
    d.iconFont = CreateFontIndirectW(&lf);

    // measure: the text wraps at 440 px; buttons at least 96 px wide
    HDC sdc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(sdc, d.font);
    RECT tr{0, 0, 440, 0};
    DrawTextW(sdc, text.c_str(), (int)text.size(), &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    std::vector<int> bw;
    int btnTotal = 0;
    for (const Btn& b : btns) {
        std::wstring s;
        for (const wchar_t* p = b.text; *p; ++p) if (*p != L'&') s += *p;
        SIZE sz{};
        GetTextExtentPoint32W(sdc, s.c_str(), (int)s.size(), &sz);
        bw.push_back(std::max(96, (int)sz.cx + 32));
        btnTotal += bw.back() + 8;
    }
    btnTotal -= 8;
    SelectObject(sdc, old);
    ReleaseDC(nullptr, sdc);
    const int m = 24, icon = d.iconKind ? 32 : 0, tx = m + (icon ? icon + 16 : 0);
    const int textW = std::max(200, (int)tr.right), textH = std::max(16, (int)tr.bottom);
    const int contentH = std::max(textH, icon);
    d.footerY = m + contentH + m;
    const int footerH = 30 + 2 * 18;
    const int cw = std::max(tx + textW + m, btnTotal + 2 * m), ch = d.footerY + footerH;
    d.iconRc = RECT{m, m, m + icon, m + icon};

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    const DWORD ex = WS_EX_DLGMODALFRAME | (owner ? 0 : WS_EX_TOPMOST | WS_EX_APPWINDOW);
    RECT wr{0, 0, cw, ch};
    AdjustWindowRectEx(&wr, style, FALSE, ex);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    RECT area;
    if (owner && IsWindowVisible(owner) && !IsIconic(owner)) GetWindowRect(owner, &area);
    else {
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
        area = mi.rcWork;
    }
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&area, MONITOR_DEFAULTTONEAREST), &mi);
    int x = (area.left + area.right - ww) / 2, y = (area.top + area.bottom - wh) / 2;
    x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - ww));
    y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - wh));
    HWND dlg = CreateWindowExW(ex, kMessageClass, title.c_str(), style, x, y, ww, wh, owner, nullptr, GetModuleHandleW(nullptr), &d);
    if (!dlg) {
        DeleteObject(d.font);
        DeleteObject(d.iconFont);
        return MessageBoxW(owner, text.c_str(), title.c_str(), flags);
    }
    ApplyWindow(dlg);
    const int ty = icon && textH < icon ? m + (icon - textH) / 2 : m;
    HWND st = CreateWindowExW(0, kStatic, text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, tx, ty, textW, textH, dlg,
                              (HMENU)(INT_PTR)0xFFFF, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(st, WM_SETFONT, (WPARAM)d.font, FALSE);
    int bx = cw - m - btnTotal;
    HWND defBtn = nullptr;
    for (size_t i = 0; i < btns.size(); ++i) {
        const bool isDef = btns[i].id == d.defId;
        HWND b = CreateWindowExW(0, kButton, btns[i].text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (isDef ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                 bx, d.footerY + 18, bw[i], 30, dlg, (HMENU)(INT_PTR)btns[i].id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)d.font, FALSE);
        if (isDef) defBtn = b;
        bx += bw[i] + 8;
    }
    if (!d.hasCancel && (flags & MB_TYPEMASK) != MB_OK) EnableMenuItem(GetSystemMenu(dlg, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
    const bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    if (flags & MB_SETFOREGROUND) SetForegroundWindow(dlg);
    if (defBtn) SetFocus(defBtn);
    MessageBeep(d.iconKind == 3 ? MB_ICONHAND : d.iconKind == 2 ? MB_ICONEXCLAMATION : d.iconKind == 4 ? MB_ICONQUESTION
                : d.iconKind == 1 ? MB_ICONASTERISK : MB_OK);
    MSG msg;
    bool quit = false;
    int quitCode = 0;
    while (!d.done && IsWindow(dlg)) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) { quit = true; quitCode = (int)msg.wParam; break; }
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (ownerWasEnabled) EnableWindow(owner, TRUE);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (owner && IsWindow(owner)) SetActiveWindow(owner);
    DeleteObject(d.font);
    DeleteObject(d.iconFont);
    if (quit) PostQuitMessage(quitCode);
    return d.result ? d.result : (d.hasCancel ? IDCANCEL : IDNO);
}

}  // namespace glass
