// steamvr_ui -- the tray's "SteamVR games" window (Win32 controls, no frameworks) and the repair check: the games
// where the VR EyeCapture SteamVR proxy can be or is enabled (tray/steamvr_games.h does the file work; this file is only
// the window, the questions, the background Steam scan and the logging).
//
// Rows: one per game = the games the user enabled ([game] in <install dir>\steamvr_games.ini) + the SteamVR games of the
// last Steam library scan ([found], the cache), re-inspected on disk at every refresh. Each row has a check box (ticked =
// the user enabled it) and a state: On / Off / Needs repair (game updated / VR EyeCapture moved / older recorder version) /
// OpenComposite installed / 32-bit, not supported / original missing / folder not found. Text "Game\tState\tFolder" (LB_GETTEXT works cross-process for tests).
//   - Ticking a box = Enable (confirmation listing every DLL hit, 32-bit skipped, anti-cheat warning with No as default,
//     OpenComposite found: offer to restore SteamVR's DLL first); unticking = Disable (confirmation). Space toggles too.
//   - Enable all: every Off game without anti-cheat (those and OpenComposite games are listed as skipped), one confirmation.
//   - Add a game by exe...: exe picker -> GameRootForExe -> the same Enable flow (non-Steam games).
//   - Disable / Re-enable: the selected game. Rescan: the Steam library again.
//   - Repair: every folder of the enabled games that needs one (steamvr::FindRepairs: game updated, older recorder, VR
//     EyeCapture moved) is enabled again (steamvr::RepairEnabledGames; a running game is skipped), Home key refreshed.
//     Nothing is repaired without this click (or Re-enable): the tray start only detects (CheckRepairs, one balloon).
//   - Disable all: one confirmation listing the enabled games, then every one is disabled. When the last enabled game is
//     disabled (here, by Disable / untick, or DisableAllGames) the HKCU Home value is deleted.
//   - Recording mode (top): steamvr_mode in config.ini, written in place at once (per game / quick mode / off; quick mode =
//     EyeCaptureMirror.exe records SteamVR's mirror picture while SteamVR runs, src/openvr/mirror_main.cpp). It lives here and
//     not in the Settings window because that window must fit a 1080p screen without scrolling (no row was free).
// The Steam scan (SteamInstallDir -> SteamLibraries -> DiscoverSteamGames) runs on a worker thread when the window opens
// and on Rescan ("Scanning the Steam library..." meanwhile); its result is written to the [found] cache.
// The proxy source is FindProxySource(install dir); the sidecar gets home = the install dir, game = the display name.
// Every question / result box is also logged ("tray: steamvr: box: ..."). Test mode: the exe picker reads the first line of
// the file named by EYECAP_TRAY_TEST_PICK, the Steam folder is EYECAP_TRAY_TEST_STEAM_DIR only; tests click the real boxes.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace steamvr_ui {

struct Options {
    std::wstring installDir;   // the tray's folder: sidecar home, steamvr_games.ini, the proxy source
    std::wstring hotkey;       // the recording hotkey text (for the "enabled" box); empty = not named
    bool testMode = false;     // EYECAP_TRAY_TEST_SHM: test picker / test Steam folder
    std::wstring configPath;   // the config.ini the tray uses: the "Recording mode" drop-down writes steamvr_mode there
};

// Modal-style window (own message loop; the tray keeps running). Only focuses the window when it is already open.
// Runs CheckRepairs first (read-only, logged; "N game(s) need a repair (...): click Repair." in the status line).
void ShowDialog(const Options& o);
HWND DialogWindow();   // the open window, or nullptr

// Read-only repair check of the games the user enabled (steamvr::FindRepairs with FindProxySource's version and home =
// installDir): every folder that needs a repair is logged ("tray: steamvr: repair check (<why>): <game>, <dir>: needs repair
// (<reason>)"). Nothing is written anywhere. games = how many games need one, names = "A, B and C".
struct RepairCheck {
    size_t games = 0, folders = 0;
    std::wstring names;
};
RepairCheck CheckRepairs(const std::wstring& installDir, const char* why);

// The [game] entries of <installDir>\steamvr_games.ini (the games the user enabled), by display name.
std::vector<std::wstring> EnabledGameNames(const std::wstring& installDir);
// Disables every enabled game (no question: the caller asked; the tray's Unregister flow), logs it, deletes the Home value when
// none is left, refreshes an open window. Returns the text for the result balloon / box; *allOk = every game was disabled.
std::wstring DisableAllGames(const std::wstring& installDir, bool testMode, const char* why, bool* allOk = nullptr);

// Our openvr_api.dll to install: <installDir>\openvr_api.dll (the tray runs from build\Release), then
// <installDir>\steamvr\openvr_api.dll (the release zip), then <installDir>\build\Release\openvr_api.dll (the dev tree).
// Only a 64-bit VR EyeCapture proxy counts. Empty = none; *tried (optional) lists the paths looked at.
std::wstring FindProxySource(const std::wstring& installDir, std::wstring* tried = nullptr);

constexpr const wchar_t kWindowTitle[] = L"VR EyeCapture - SteamVR games";
constexpr const wchar_t kWindowClass[] = L"EyeCaptureSteamVR";

// Control IDs, so tests can drive the window through SendMessage / PostMessage.
enum : int {
    ID_SVR_INTRO = 3001,   // explanation text
    ID_SVR_LIST,           // owner-drawn list box with check boxes: one row per game, "Game\tState\tFolder"
    ID_SVR_DETAILS,        // read-only edit: the selected game's exe, folder and every DLL folder in full
    ID_SVR_ENABLE,         // "Add a game by exe..."
    ID_SVR_DISABLE,        // "Disable" (selected game)
    ID_SVR_REENABLE,       // "Re-enable" (selected game)
    ID_SVR_CLOSE,          // "Close"
    ID_SVR_HEADER,         // the column header line above the list
    ID_SVR_ENABLEALL,      // "Enable all"
    ID_SVR_RESCAN,         // "Rescan"
    ID_SVR_STATUS,         // status line: "Scanning the Steam library..." / "Steam library: ..." / repair results
    ID_SVR_TOGGLE,         // not a control: WM_COMMAND with this id toggles the selected row's check box (= a click on it)
    ID_SVR_MODE,           // "Recording mode" drop-down: steamvr_mode = per_game | quick | off, written to config.ini at once
    ID_SVR_REPAIR,         // "Repair": every enabled game that needs a repair (enabled only when one does)
    ID_SVR_DISABLEALL,     // "Disable all": every enabled game, one confirmation
};

} // namespace steamvr_ui
