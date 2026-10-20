// update -- the tray app's update checker and one-click updater.
//
// Split in two halves so the first can be unit-tested with no network (tests/update_test.cpp):
//   * pure logic: semantic version parse / compare, the GitHub "releases/latest" JSON parse, the asset-name wildcard,
//     the download-host allow-list;
//   * networking (WinHTTP): the HTTPS GET of the API, the asset download (redirects followed only to the allowed hosts)
//     and the SHA-256 of the downloaded file.
//
// Nothing here is started on its own: the tray checks GitHub only while check_updates = 1 (at start after a short delay,
// then at most once a day) and downloads / runs the installer only on the user's click (docs/DEVELOPMENT.md "Antivirus").
#pragma once
#include <windows.h>
#include <functional>
#include <string>

namespace upd {

// api.github.com endpoint for this project's latest release. Overridable for tests / a mock by the environment variable
// XR_EYE_CAPTURE_UPDATE_URL (so a test can point the tray at a local server without touching GitHub).
constexpr const wchar_t kDefaultApiUrl[] = L"https://api.github.com/repos/JRDevo/VR-EyeCapture/releases/latest";
// The release asset the updater downloads (Inno Setup installer; see the task / README "Updates"). Case-insensitive glob.
constexpr const char kAssetGlob[] = "VR-EyeCapture-Setup-v*.exe";

// Semantic version X.Y.Z (anything after the patch -- e.g. a "-beta" suffix -- is ignored by the compare).
struct Version { int major = 0, minor = 0, patch = 0; };
// Parses "vX.Y.Z" or "X.Y.Z" (a leading 'v' / 'V' is optional; a 4th component or a "-suffix" after Z is ignored).
// False when there are not at least major.minor.patch numeric fields.
bool ParseVersion(const std::string& tag, Version& out);
// -1 if a < b, 0 if equal, 1 if a > b (major, then minor, then patch).
int CompareVersions(const Version& a, const Version& b);
std::string VersionText(const Version& v);   // "X.Y.Z"

// What we need from the GitHub "releases/latest" response.
struct ReleaseInfo {
    bool        ok = false;        // JSON parsed, release is not a draft / prerelease, and a matching asset was found
    bool        draft = false;
    bool        prerelease = false;
    std::string tag;               // raw tag_name, e.g. "v0.6.0"
    Version     version;           // parsed from tag
    std::string htmlUrl;           // the release page (release notes), for the confirm dialog
    std::string assetName;         // the matched asset's file name
    std::string assetUrl;          // its browser_download_url
    std::string digestHex;         // lower-case hex SHA-256 taken from the asset "digest":"sha256:..." ("" if absent)
    std::string error;             // why ok is false (logged)
};
// Parses the API body. assetGlob is a case-insensitive '*' / '?' wildcard (kAssetGlob). A draft or prerelease release, a
// missing / unparsable tag, or no matching asset all give ok = false with `error` set; the fields that were read are
// still filled in where possible. A missing digest is NOT an error here (ok can be true with an empty digestHex); the
// download step refuses an empty / mismatched digest.
bool ParseLatestRelease(const std::string& json, const std::string& assetGlob, ReleaseInfo& out);

// Whole-string, case-insensitive wildcard match ('*' = any run, '?' = one char).
bool WildcardMatch(const std::string& text, const std::string& pattern);

// true only for an https URL whose host is github.com, objects.githubusercontent.com, or any *.githubusercontent.com
// (release-assets.githubusercontent.com); everything else (http, any other host) is rejected. Used for the asset URL and
// for every redirect target during the download.
bool IsAllowedDownloadHost(const std::wstring& url);

// true only for a URL that begins exactly with "https://github.com/" (case-insensitive scheme + host). Used before the
// tray opens the release-notes link (html_url) in the browser, so a tampered response cannot make it launch anything else.
bool IsGithubUrl(const std::string& url);

// ---- networking (WinHTTP); not exercised by the unit test ------------------------------------------------------------

// HTTPS GET of `url` with the given User-Agent and an Accept: application/vnd.github+json header; connect / send / receive
// timeouts of `timeoutMs`. Returns the body (UTF-8) in `body`. false on any failure (err set); `statusOut` (optional) gets
// the HTTP status code.
bool HttpGet(const std::wstring& url, const std::wstring& userAgent, std::string& body, std::wstring& err,
             DWORD timeoutMs = 10000, int* statusOut = nullptr);

// Downloads `url` to `destPath`. Redirects are followed manually and ONLY to IsAllowedDownloadHost hosts (anything else
// aborts the download). `progress` (optional) is called with (bytesSoFar, totalBytes; total 0 = unknown). false on any
// failure (err set; a partial output file is deleted).
bool DownloadFile(const std::wstring& url, const std::wstring& destPath, const std::wstring& userAgent, std::wstring& err,
                  std::function<void(unsigned long long, unsigned long long)> progress = nullptr);

// SHA-256 of a file as lower-case hex (CNG / BCrypt). "" on error.
std::string Sha256Hex(const std::wstring& path);

} // namespace upd
