// glass -- the Windows 11 look of the tray's windows (Settings, SteamVR games), drawn with our own Win32 controls.
//
// Theme: light / dark from HKCU ...\Themes\Personalize AppsUseLightTheme, the accent colour from HKCU ...\Explorer\Accent
// AccentPalette (DwmGetColorizationColor as fallback), transparency from EnableTransparency -- all READ only. Windows 11
// (build 22621+) with transparency on: Mica backdrop (DWMWA_SYSTEMBACKDROP_TYPE) behind the whole client area, which is
// part of the DWM frame (DwmExtendFrameIntoClientArea); otherwise (Windows 10, transparency off) a plain solid background.
// The theme follows Windows at run time: a window that calls glass::OnSettingChange from WM_SETTINGCHANGE /
// WM_DWMCOLORIZATIONCOLORCHANGED repaints itself and every glass control in it.
//
// Controls (window classes registered by glass::Init): they paint into a 32-bit buffer (BufferedPaint) with correct alpha,
// so the Mica shows through where they are transparent; text is drawn with DrawThemeTextEx(DTT_COMPOSITED) in the window's
// font (WM_SETFONT), so text widths are the GDI widths the layout code measures.
//   kButton  "GlassButton"  styles like BUTTON: BS_PUSHBUTTON / BS_DEFPUSHBUTTON (accent fill), BS_AUTOCHECKBOX, BS_GROUPBOX
//            (a card with a title). Messages: BM_GETCHECK / BM_SETCHECK / BM_CLICK, WM_SETTEXT; sends WM_COMMAND BN_CLICKED.
//   kCombo   "GlassCombo"   a drop-down list (CBS_DROPDOWNLIST behaviour) with its own popup. CB_ADDSTRING, CB_RESETCONTENT,
//            CB_GETCOUNT, CB_GETCURSEL, CB_SETCURSEL, CB_GETLBTEXT, CB_GETLBTEXTLEN, CB_FINDSTRINGEXACT, CB_SETDROPPEDWIDTH,
//            CB_SHOWDROPDOWN, CB_GETDROPPEDSTATE, CB_GETITEMDATA / CB_SETITEMDATA, CB_DELETESTRING, CB_INSERTSTRING;
//            sends CBN_SELCHANGE (user picks only) / CBN_DROPDOWN / CBN_CLOSEUP.
//   kStatic  "GlassStatic"  text (SS_LEFT / SS_CENTER / SS_RIGHT, SS_ENDELLIPSIS, SS_PATHELLIPSIS, multi-line by default);
//            glass::SetDim / SetTextColor pick grey / any colour.
//   kSlider  "GlassSlider"  a horizontal trackbar: TBM_SETRANGE / GETPOS / SETPOS / SETPAGESIZE / SETLINESIZE / SETTICFREQ;
//            sends WM_HSCROLL (TB_THUMBTRACK / TB_ENDTRACK / TB_LINEUP ...).
//   kLink    "GlassLink"    SysLink subset: text with <a href="url">..</a> parts (LWS_RIGHT); WM_NOTIFY NM_CLICK / NM_RETURN
//            with an NMLINK (item.iLink, item.szUrl).
//   kProgress "GlassProgress" PBM_SETRANGE(32) / SETPOS / GETPOS / DELTAPOS / SETSTATE / GETSTATE (normal / paused / error).
//   kStatic with SS_OWNERDRAW: WM_DRAWITEM to the parent into a 32-bit memory bitmap (GDI); opaque, or with
//            SetOwnerDrawTransparent: drawn twice (DRAWITEMSTRUCT.itemData = the background colour to fill with, black then
//            white) to recover per-pixel alpha.
//   Edits stay native EDIT controls: glass::FrameEdit gives one the Windows 11 field look (rounded field, accent underline
//   while focused) by subclassing it; its own text area is opaque.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace glass {

extern const wchar_t* const kButton;
extern const wchar_t* const kCombo;
extern const wchar_t* const kStatic;
extern const wchar_t* const kSlider;
extern const wchar_t* const kLink;
extern const wchar_t* const kProgress;

// Geometry the layout code relies on: a check box's text starts kCheckTextX px from its left edge; a closed drop-down shows
// its text in (width - kComboTextPad) px.
const int kCheckTextX = 22;
const int kComboTextPad = 31;

struct Theme {
    bool dark = false;
    bool mica = false;            // the Mica backdrop is on (Windows 11 + transparency); else a solid background
    COLORREF accent = RGB(0, 103, 192);         // fills (default button, checked box, slider)
    COLORREF accentText = RGB(255, 255, 255);   // text / glyphs on the accent
    COLORREF text = RGB(27, 27, 27), textDim = RGB(96, 96, 96), textDisabled = RGB(160, 160, 160);
    COLORREF window = RGB(243, 243, 243);       // solid background (no Mica) -- also what the title bar shows
};

// Once per process (any thread that makes glass windows): GDI+ / class registration. Safe to call again.
void Init();
const Theme& Current();
// Re-reads the theme from the registry. true = something changed.
bool Reload();
// Test / screenshot hook: force light (0) / dark (1) / follow Windows (-1).
void ForceDark(int mode);
// Test / screenshot hook: force the solid background (0) / Mica where available (1) / follow Windows (-1).
void ForceMica(int mode);

// A top-level window: dark title bar to match, rounded corners, Mica (or solid) background. Call after CreateWindowEx and
// again after the theme changed. The window's WM_ERASEBKGND must call PaintBackground.
void ApplyWindow(HWND top);
// Fills `rc` of a top-level window's client with the background: transparent (alpha 0, the Mica shows) or solid.
void PaintBackground(HWND top, HDC dc, const RECT& rc);
// Popup menus (TrackPopupMenu) of this process in the current light / dark theme. Call right before showing one. Uses
// uxtheme's unnamed SetPreferredAppMode / FlushMenuThemes (ordinals 135 / 136, Windows 10 1903+, what Explorer uses);
// without them menus stay light.
void ThemeMenus();
// From the window procedure of a top-level glass window on WM_SETTINGCHANGE / WM_DWMCOLORIZATIONCOLORCHANGED /
// WM_THEMECHANGED: reloads the theme and repaints. Returns true when the theme changed.
bool OnSettingChange(HWND top);
// After ForceDark: re-applies the window look and repaints every control in it (also used by OnSettingChange).
void Repaint(HWND top);

// Grey text (hints, captions) / an explicit colour for a kStatic (CLR_INVALID = the theme's normal text colour).
void SetDim(HWND stat, bool dim);
void SetTextColor(HWND stat, COLORREF c);
// A kStatic drawn as a layer strip (the footer behind the bottom buttons).
void SetPanel(HWND stat);
// A kStatic SS_OWNERDRAW whose drawing fills its background with DRAWITEMSTRUCT.itemData (see above).
void SetOwnerDrawTransparent(HWND stat);
// Native EDIT / LISTBOX: the Windows 11 field look (subclass; layered over Mica so its GDI text stays opaque). Call once
// after creating it.
void FrameEdit(HWND edit);
// The brush / colours a parent returns for WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC of a framed edit (read-only too).
HBRUSH EditBrush(HDC dc, HWND edit);

// Owner-drawn rows of a framed (FrameEdit) list box: the field colour, a selected row's tint and the text colour; and a
// Windows 11 check box drawn into `box` (GDI+, anti-aliased) on an opaque surface.
COLORREF FieldBg();
void ListRowColors(bool selected, bool focused, COLORREF* bg, COLORREF* fg);
void DrawCheckBox(HDC dc, const RECT& box, bool checked, bool enabled);

// Own message dialog in the glass look (MessageBoxW subset: MB_OK / MB_OKCANCEL / MB_YESNO / MB_YESNOCANCEL, MB_ICON*,
// MB_DEFBUTTON1..3). Window class kMessageClass, title `title`; returns IDOK / IDCANCEL / IDYES / IDNO.
extern const wchar_t* const kMessageClass;
int MessageBox(HWND owner, const std::wstring& text, const std::wstring& title, UINT flags);

}  // namespace glass
