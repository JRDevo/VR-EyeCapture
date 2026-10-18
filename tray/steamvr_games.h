// steamvr_games -- the tray's "SteamVR games" bookkeeping, UI-free (the menu / dialog that calls it is separate):
// find a game's openvr_api.dll copies, tell what each one is (Valve's / our proxy / OpenComposite / 32-bit), enable the
// VR EyeCapture proxy there (Valve's DLL renamed in place to openvr_api_real.dll, our proxy copied as openvr_api.dll, a
// sidecar eye_capture_proxy.ini with home = the install folder), disable it again, and keep the list of enabled games in
// <install dir>\steamvr_games.ini (bookkeeping only: the files on disk are the truth). See docs/STEAMVR_DESIGN.md.
//
// No logging and no UI here: every call returns what it did / why it refused, in words for the dialog and a Win32 error.
// Writes only inside the game folder the caller passed (and the ini at the path the caller passed).
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace steamvr {

enum class Machine { NotPe, X64, X86, Other };
enum class DllKind {
    Missing,        // no such file
    Valve,          // Valve's openvr_api.dll (exports VR_InitInternal2 + VR_GetGenericInterface, none forwarded, no marker)
    Proxy,          // ours (exports EyeCaptureProxyVersion)
    OpenComposite,  // OpenComposite (the "OpenComposite" text in the file, or exports forwarded to *_oc)
    Unknown,        // a PE file that is none of the above
    NotPe,
};
const char* DllKindName(DllKind k);

struct DllInfo {
    std::wstring path;
    Machine      machine = Machine::NotPe;
    DllKind      kind = DllKind::Missing;
    std::string  proxyVersion;   // Proxy: the version string next to the EyeCaptureProxyVersion export ("" if unknown)
    std::wstring company, fileVersion;   // version resource (Valve's: "Valve", "1.13.10")
    std::vector<std::string> exports;    // export names (forwarded ones as "name -> module.func")
};
DllInfo InspectDll(const std::wstring& path);

// One folder of a game that has (or had) an openvr_api.dll.
enum class LocState {
    NotEnabled,        // Valve's DLL, no proxy
    Enabled,           // our proxy + openvr_api_real.dll (Valve's)
    ProxyMissing,      // Valve's DLL is back (Steam update / verify files) but our real / sidecar are still there: Enable again
    OriginalMissing,   // our proxy, but openvr_api_real.dll is gone
    OpenComposite,     // openvr_api.dll is OpenComposite (or an openvr_api_oc.dll is there): restore SteamVR's DLL first
    Unsupported32,     // a 32-bit openvr_api.dll: not supported, skipped
    Unknown,           // not Valve's, not ours
};
const char* LocStateName(LocState s);   // short words for the list: "enabled", "proxy missing -- game updated? Enable again", ...

struct Location {
    std::wstring dir;            // folder of the DLL (no trailing slash)
    DllInfo      dll;            // <dir>\openvr_api.dll
    DllInfo      real;           // <dir>\openvr_api_real.dll (kind Missing when absent)
    bool         hasSidecar = false;
    std::wstring sidecarHome;
    bool         hasOcDll = false;           // <dir>\openvr_api_oc.dll (OpenComposite's renamed Valve DLL)
    bool         hasSteamVrBackup = false;   // <dir>\openvr_api.dll.steamvr-backup (OpenComposite switcher's copy of Valve's)
    LocState     state = LocState::Unknown;
    bool         proxyOutdated = false;      // Enabled with a proxy OLDER than `ourVersion` of the scan (VersionLess)
    // openvr_api.dll.eyecapture-old / -new left by an interrupted Enable / Disable: InspectLocation deletes them when nothing
    // holds them (staleRemoved), else lists them (staleKept: the game still has the renamed proxy loaded)
    std::vector<std::wstring> staleRemoved, staleKept;
};

struct ScanResult {
    std::wstring root;                       // the folder that was scanned
    std::vector<Location> locations;         // every folder with an openvr_api.dll or openvr_api_real.dll (64- and 32-bit)
    std::vector<std::wstring> antiCheat;     // EasyAntiCheat / BattlEye files or folders found (warn, the user decides)
    bool truncated = false;                  // the scan stopped at its file / depth limit
};
// The folder to scan for a game exe: the Steam library folder steamapps\common\<Game> when the exe is inside one, else
// for UE layouts (<Game>\Binaries\Win64\x.exe) the folder above <Game>, else the exe's folder.
std::wstring GameRootForExe(const std::wstring& exePath);
// Recursive (no reparse points, depth <= maxDepth, <= maxEntries entries, and -- timeBudgetMs > 0 -- stops after that many
// ms; any limit hit sets truncated). ourVersion: our proxy's version (proxyOutdated).
ScanResult ScanGame(const std::wstring& root, const std::string& ourVersion, int maxDepth = 12, size_t maxEntries = 200000,
                    unsigned timeBudgetMs = 0);
// Re-inspects one folder (after an action).
Location InspectLocation(const std::wstring& dir, const std::string& ourVersion);

struct ActionResult {
    bool ok = false;
    DWORD error = 0;                  // Win32 error of the step that failed (0 = refused for a reason in `message`)
    std::wstring message;             // what was done / why not (dialog text)
    std::vector<std::wstring> steps;  // file operations done, in order ("renamed X -> Y", "copied ...", "wrote ...")
};
// ---- checks every Enable / Disable runs first
enum class FileAccess { Missing, Free, InUse, Denied };
// Free = can be opened for write + delete. InUse = a sharing / lock violation: a running game has the DLL mapped (Windows
// lets a loaded DLL be RENAMED but never deleted or overwritten). Denied = ERROR_ACCESS_DENIED: a protected folder that
// needs administrator rights, or a read-only file.
FileAccess CheckFileAccess(const std::wstring& path);
// The DLL whose exports the proxy must cover: Valve's openvr_api.dll (NotEnabled; ProxyMissing when it is back) or the
// renamed openvr_api_real.dll (Enabled; ProxyMissing without a DLL). nullptr for the states Enable refuses anyway.
const DllInfo* GameDllOf(const Location& loc);
// Export names of `game` (forwarders count by name) that `proxy` lacks. A game DLL newer than the proxy's export list would
// make the game fail to load -- Enable / repair refuse it (MissingExportsText).
std::vector<std::string> MissingExports(const DllInfo& game, const DllInfo& proxy);
std::wstring MissingExportsText(const std::vector<std::string>& missing);
// FileAccess InUse / Denied on openvr_api.dll or openvr_api_real.dll: sets r.error / r.message ("... is in use -- close the
// game ...; nothing was changed") and returns true.
bool RefuseInUse(const Location& loc, ActionResult& r);

// Enables the proxy in one folder. proxySource = our openvr_api.dll (the install's steamvr\openvr_api.dll), home / game =
// the sidecar values. Valve's DLL -> renamed openvr_api_real.dll; a ProxyMissing folder -> the updated Valve DLL replaces
// the old openvr_api_real.dll; Enabled -> the proxy and the sidecar are refreshed (idempotent). Refused before any file
// operation: 32-bit, OpenComposite / unknown DLL (RestoreSteamVrDll first), original missing, a game DLL with an export the
// proxy lacks (MissingExports), a DLL in use / access denied (RefuseInUse). A failed step rolls back what was done.
// ok == true never comes with an error set: a cleanup that failed after the work is mentioned in `message` only.
ActionResult EnableLocation(const Location& loc, const std::wstring& proxySource, const std::wstring& home, const std::wstring& game);
// Removes the proxy + sidecar and renames openvr_api_real.dll back (refused first while a DLL is in use, RefuseInUse). ProxyMissing: only the stale real copy + sidecar go.
// Not enabled: nothing to do (ok). Original missing: refused unless openvr_api.dll.steamvr-backup is Valve's (then that
// one is copied back).
ActionResult DisableLocation(const Location& loc);
// OpenComposite installed in this folder: keeps it as openvr_api.dll.opencomposite (rename) and copies
// openvr_api.dll.steamvr-backup (must be Valve's) back to openvr_api.dll, so Enable can run.
ActionResult RestoreSteamVrDll(const Location& loc);

// ---- <install dir>\steamvr_games.ini
// [game] sections = the games the USER enabled (only these are ever repaired); [found] sections = the cache of the last
// Steam library scan (FoundGame). A section header other than these ends the current section (its keys are ignored).
struct GameEntry {
    std::wstring name;                // display name
    std::wstring exe;                 // the exe the user picked ("" for a game enabled from the Steam list)
    std::wstring root;                // the scanned folder
    std::vector<std::wstring> dlls;   // folders where the proxy was enabled
    std::wstring appid;               // Steam app id ("" = added by exe)
};
bool LoadGameList(const std::wstring& iniPath, std::vector<GameEntry>& out);
// temp file + rename. found == nullptr: the [found] sections already in the file are kept as they are.
struct FoundGame;
bool SaveGameList(const std::wstring& iniPath, const std::vector<GameEntry>& games, const std::vector<FoundGame>* found = nullptr);
// Adds or replaces the entry with the same root (case-insensitive).
void UpsertGame(std::vector<GameEntry>& games, const GameEntry& g);
bool RemoveGame(std::vector<GameEntry>& games, const std::wstring& root);
// Display name for an exe ("Crisis VRigade 2" from "...\Crisis VRigade 2.exe").
std::wstring GameNameForExe(const std::wstring& exePath);

// "0.4.0" < "0.4.10" < "0.5": dotted numbers compared numerically (missing parts = 0; non-digits end a part).
bool VersionLess(const std::string& a, const std::string& b);

// ---- Steam library discovery (read-only; no Steam API, just Steam's own text files)
// Valve KeyValues text (libraryfolders.vdf, appmanifest_*.acf): "key" "value" / "key" { ... }, // comments, \ " 

// escapes. A small tree; Get() = the first child with that key (case-insensitive).
struct KvNode {
    std::wstring key, value;           // value: for a leaf
    bool isBlock = false;
    std::vector<KvNode> children;
    const KvNode* Get(const wchar_t* k) const;
};
bool ParseKeyValues(const std::string& utf8, KvNode& root);   // root = an unnamed block holding the top-level pairs
// The library folders listed in libraryfolders.vdf: new format ("libraryfolders" { "0" { "path" "..." } }) and the old
// one ("LibraryFolders" { "1" "D:\\SteamLibrary" }). Paths with '/' turned into '\', no trailing slash, duplicates dropped.
std::vector<std::wstring> ParseLibraryFolders(const std::string& vdfUtf8);
struct AppManifest { std::wstring appid, name, installdir; };
bool ParseAppManifest(const std::string& acfUtf8, AppManifest& out);   // false = no AppState / appid / installdir
// Steam's install folder: HKCU\Software\Valve\Steam SteamPath, else HKLM\SOFTWARE\WOW6432Node\Valve\Steam InstallPath
// ("" = none). testOverride (test-mode tray): ONLY EYECAP_TRAY_TEST_STEAM_DIR counts ("" when it is not set), so a test
// never reads the real Steam folders.
std::wstring SteamInstallDir(bool testOverride);
// Every library: <steamDir> itself first, then libraryfolders.vdf's (steamapps\libraryfolders.vdf, else config\...).
std::vector<std::wstring> SteamLibraries(const std::wstring& steamDir);

// One installed Steam game that has an openvr_api.dll somewhere in its folder (64- or 32-bit).
struct FoundGame {
    std::wstring appid, name, root;          // root = <library>\steamapps\common\<installdir>
    std::vector<std::wstring> dlls;          // folders with a 64-bit openvr_api.dll (or openvr_api_real.dll)
    std::vector<std::wstring> dlls32;        // folders with only a 32-bit one
    bool antiCheat = false;                  // EasyAntiCheat / BattlEye seen in the folder
    bool truncated = false;                  // the folder scan hit a limit (a huge game): it may have more
};
bool LoadFoundGames(const std::wstring& iniPath, std::vector<FoundGame>& out);
// Every appmanifest of every library -> a cheap ScanGame(<root>, depth 3, 20000 entries, 1 s); a deep one (depth 10, 60000
// entries, 4 s) only when the cheap one hit a limit or the folder has a Unity (<Game>_Data) / Unreal (Engine) layout within
// two levels -> the games with an openvr_api.dll. cancel (optional) is polled between games; totalBudgetMs (0 = none) ends
// the whole scan early (*budgetHit = true: later games are missing). Sorted by name.
std::vector<FoundGame> DiscoverSteamGames(const std::vector<std::wstring>& libraries, const std::string& ourVersion,
                                          const volatile bool* cancel = nullptr, bool* budgetHit = nullptr,
                                          unsigned totalBudgetMs = 90000);

// ---- repair of the games the user enabled: the tray only DETECTS at start (FindRepairs, read-only, one balloon); a game
// folder is changed only on the user's click (the SteamVR games window's Repair / Re-enable: RepairEnabledGames / EnableLocation).
// CheckFileAccess(path) == InUse (a running game has it mapped as an image, or another handle denies sharing). A missing
// file or an access-denied folder is not "in use".
bool FileInUse(const std::wstring& path);
enum class RepairWhy {
    None,
    GameUpdated,     // ProxyMissing: a game update / "verify files" put Valve's DLL back while our real copy / sidecar stayed
    ProxyOutdated,   // Enabled with a proxy older than ourVersion (a newer VR EyeCapture)
    Moved,           // Enabled, but the sidecar's home is not this install folder (or no longer exists): VR EyeCapture moved
};
const char* RepairWhyName(RepairWhy w);   // "game updated" / "older recorder version" / "VR EyeCapture moved" / "none"
// What one folder needs. home = this install folder ("" = the Moved check is skipped); compared as full paths, ignoring case
// and a trailing slash. Moved wins over ProxyOutdated (the same repair fixes both).
RepairWhy RepairNeeded(const Location& L, const std::wstring& home);
struct RepairNeed {
    RepairWhy why = RepairWhy::None;
    std::wstring game, dir;
};
// Read-only: every existing folder of every game in `games` (never any other folder) whose RepairNeeded != None.
std::vector<RepairNeed> FindRepairs(const std::vector<GameEntry>& games, const std::wstring& home, const std::string& ourVersion);
struct RepairItem {
    enum class What { Repaired, Updated, Moved, SkippedInUse, Failed } what = What::Failed;
    std::wstring game, dir, message;
};
// Acts on what FindRepairs reports -- call it only on the user's click: GameUpdated -> EnableLocation = Repaired (the updated
// Valve DLL becomes openvr_api_real.dll); ProxyOutdated -> Updated; Moved -> Moved (proxy and sidecar rewritten with home =
// `home`). A folder whose openvr_api.dll / openvr_api_real.dll is in use (FileInUse: the game runs) is skipped
// (SkippedInUse, nothing changed). Folders that need nothing are not touched and not reported. The export / access checks of
// EnableLocation apply (a refusal = Failed with its message).
std::vector<RepairItem> RepairEnabledGames(const std::vector<GameEntry>& games, const std::wstring& proxySource,
                                           const std::wstring& home, const std::string& ourVersion);

} // namespace steamvr
