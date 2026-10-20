// update_test -- CPU unit test of the tray's update checker logic (tray/update): semantic version parse / compare, the
// GitHub "releases/latest" JSON parse (including a fixture with an asset digest), the installer-asset wildcard and the
// download-host allow-list. No network by default. Exit code 0 = all checks passed.
//
// `update_test.exe --live` adds an OPT-IN live test (not run by default or in CI): it GETs the real GitHub API, downloads
// the v0.5.0 Setup asset, verifies its SHA-256, and deletes it. It never executes the downloaded file.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "update.h"

#ifndef EYE_TEST_FIXTURES
#define EYE_TEST_FIXTURES "tests/fixtures"
#endif

namespace {
int g_fail = 0;
void Check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}
std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
upd::Version V(int a, int b, int c) { upd::Version v; v.major = a; v.minor = b; v.patch = c; return v; }
std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Opt-in live test: hits the real GitHub API, downloads the v0.5.0 Setup asset, verifies its SHA-256, deletes it. Never
// runs by default or in CI (requires --live). It never executes the downloaded file.
int RunLive() {
    const std::string expectedDigest = "4224257d73c923eb2772ea2c463c1ceb7c24324112221f82a0a90156d1078f79";
    const std::wstring ua = L"VR-EyeCapture-test/0.5.0";
    printf("=== LIVE: GET %S\n", upd::kDefaultApiUrl);
    std::string body;
    std::wstring err;
    int status = 0;
    if (!upd::HttpGet(upd::kDefaultApiUrl, ua, body, err, 15000, &status)) {
        printf("  FAIL HttpGet: %S (status %d)\n", err.c_str(), status);
        return 1;
    }
    printf("  ok   HttpGet: HTTP %d, %zu bytes\n", status, body.size());

    upd::ReleaseInfo r;
    const bool parsed = upd::ParseLatestRelease(body, upd::kAssetGlob, r);
    Check(parsed && r.ok, "the live release parses");
    printf("  tag=%s asset=%s\n  url=%s\n  digest=%s\n", r.tag.c_str(), r.assetName.c_str(), r.assetUrl.c_str(), r.digestHex.c_str());
    Check(r.tag == "v0.5.0", "latest tag is v0.5.0");
    Check(r.assetName == "VR-EyeCapture-Setup-v0.5.0.exe", "the Setup asset is v0.5.0");
    Check(!r.digestHex.empty(), "the asset digest is non-empty");
    Check(upd::IsAllowedDownloadHost(W(r.assetUrl)), "the asset browser_download_url is an allowed host");
    Check(r.digestHex == expectedDigest, "the API digest matches the expected v0.5.0 SHA-256");
    if (g_fail) { printf("=== update_test --live FAIL (before download)\n"); return 1; }

    wchar_t tmp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"VR-EyeCapture-update-test";
    CreateDirectoryW(dir.c_str(), nullptr);   // ignore ERROR_ALREADY_EXISTS
    const std::wstring dest = dir + L"\\" + W(r.assetName);
    DeleteFileW(dest.c_str());
    printf("  downloading to %S ...\n", dest.c_str());
    if (!upd::DownloadFile(W(r.assetUrl), dest, ua, err)) {
        printf("  FAIL DownloadFile: %S\n", err.c_str());
        return 1;
    }
    // DownloadFile writes the file only if EVERY redirect host passed IsAllowedDownloadHost, so success confirms the real
    // redirect target (release-assets.githubusercontent.com / objects.githubusercontent.com) is accepted by the allow-list.
    Check(true, "DownloadFile followed the real redirect to an allowed host and wrote the file");
    LARGE_INTEGER sz{};
    HANDLE f = CreateFileW(dest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) { GetFileSizeEx(f, &sz); CloseHandle(f); }
    printf("  downloaded %lld bytes\n", (long long)sz.QuadPart);
    const std::string sha = upd::Sha256Hex(dest);
    printf("  sha256(file)=%s\n", sha.c_str());
    Check(sha == expectedDigest, "the downloaded file's SHA-256 matches the expected digest");
    Check(sha == r.digestHex, "the downloaded file's SHA-256 matches the API digest");
    DeleteFileW(dest.c_str());
    Check(GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES, "the downloaded file was deleted (never executed)");
    printf("=== update_test --live %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--live") == 0) return RunLive();
    printf("=== version parse / compare\n");
    {
        upd::Version v;
        Check(upd::ParseVersion("v0.6.0", v) && v.major == 0 && v.minor == 6 && v.patch == 0, "parse 'v0.6.0'");
        Check(upd::ParseVersion("0.5.0", v) && v.major == 0 && v.minor == 5 && v.patch == 0, "parse '0.5.0' (no leading v)");
        Check(upd::ParseVersion("V12.34.56", v) && v.major == 12 && v.minor == 34 && v.patch == 56, "parse 'V12.34.56'");
        Check(upd::ParseVersion("v1.2.3-beta.1", v) && v.major == 1 && v.minor == 2 && v.patch == 3, "parse 'v1.2.3-beta.1' (suffix ignored)");
        Check(upd::ParseVersion("v1.2.3.4", v) && v.major == 1 && v.minor == 2 && v.patch == 3, "parse 'v1.2.3.4' (4th field ignored)");
        Check(!upd::ParseVersion("v1.2", v), "reject 'v1.2' (too few fields)");
        Check(!upd::ParseVersion("", v), "reject empty");
        Check(!upd::ParseVersion("vX.Y.Z", v), "reject non-numeric");
        Check(!upd::ParseVersion("latest", v), "reject 'latest'");
    }
    {
        Check(upd::CompareVersions(V(0, 5, 0), V(0, 6, 0)) < 0, "0.5.0 < 0.6.0");
        Check(upd::CompareVersions(V(0, 6, 0), V(0, 5, 0)) > 0, "0.6.0 > 0.5.0");
        Check(upd::CompareVersions(V(0, 5, 0), V(0, 5, 0)) == 0, "0.5.0 == 0.5.0");
        Check(upd::CompareVersions(V(1, 0, 0), V(0, 99, 99)) > 0, "1.0.0 > 0.99.99 (major wins)");
        Check(upd::CompareVersions(V(0, 5, 1), V(0, 5, 0)) > 0, "0.5.1 > 0.5.0 (patch)");
        Check(upd::CompareVersions(V(0, 10, 0), V(0, 9, 0)) > 0, "0.10.0 > 0.9.0 (numeric, not lexical)");
    }

    printf("=== asset-name wildcard\n");
    {
        Check(upd::WildcardMatch("VR-EyeCapture-Setup-v0.6.0.exe", upd::kAssetGlob), "Setup installer matches the glob");
        Check(upd::WildcardMatch("VR-EYECAPTURE-SETUP-V0.6.0.EXE", upd::kAssetGlob), "glob is case-insensitive");
        Check(!upd::WildcardMatch("obs-eyecapture-v0.6.0.zip", upd::kAssetGlob), "the OBS zip does not match");
        Check(!upd::WildcardMatch("VR-EyeCapture-Setup-v0.6.0.exe.sig", upd::kAssetGlob), "a .sig suffix does not match");
        Check(upd::WildcardMatch("ab", "a?"), "'?' matches one char");
        Check(!upd::WildcardMatch("abc", "a?"), "'?' matches exactly one");
    }

    printf("=== download-host allow-list\n");
    {
        Check(upd::IsAllowedDownloadHost(L"https://github.com/JRDevo/VR-EyeCapture/releases/download/v0.6.0/x.exe"), "github.com https allowed");
        Check(upd::IsAllowedDownloadHost(L"https://objects.githubusercontent.com/abc"), "objects.githubusercontent.com allowed");
        Check(upd::IsAllowedDownloadHost(L"https://release-assets.githubusercontent.com/abc"), "release-assets.githubusercontent.com allowed");
        Check(!upd::IsAllowedDownloadHost(L"http://github.com/x.exe"), "http (not https) rejected");
        Check(!upd::IsAllowedDownloadHost(L"https://evil.com/x.exe"), "other host rejected");
        Check(!upd::IsAllowedDownloadHost(L"https://github.com.evil.com/x.exe"), "look-alike host rejected");
        Check(!upd::IsAllowedDownloadHost(L"https://notgithubusercontent.com/x"), "suffix look-alike rejected");
    }

    printf("=== release-notes URL guard (IsGithubUrl)\n");
    {
        Check(upd::IsGithubUrl("https://github.com/JRDevo/VR-EyeCapture/releases/tag/v0.6.0"), "real github.com release URL allowed");
        Check(upd::IsGithubUrl("HTTPS://GitHub.com/JRDevo/x"), "scheme + host case-insensitive");
        Check(!upd::IsGithubUrl("http://github.com/x"), "http rejected");
        Check(!upd::IsGithubUrl("https://github.com.evil.com/x"), "look-alike host rejected");
        Check(!upd::IsGithubUrl("https://evil.com/github.com/x"), "github.com elsewhere in the URL rejected");
        Check(!upd::IsGithubUrl("javascript:alert(1)"), "non-http scheme rejected");
        Check(!upd::IsGithubUrl(""), "empty rejected");
    }

    printf("=== release JSON parse (fixture with a digest)\n");
    {
        const std::string json = ReadFile(std::string(EYE_TEST_FIXTURES) + "/update/latest_release.json");
        Check(!json.empty(), "fixture latest_release.json loaded");
        upd::ReleaseInfo r;
        const bool ok = upd::ParseLatestRelease(json, upd::kAssetGlob, r);
        Check(ok && r.ok, "fixture parses and yields a usable release");
        Check(r.tag == "v0.6.0" && r.version.major == 0 && r.version.minor == 6 && r.version.patch == 0, "tag_name v0.6.0 -> 0.6.0");
        Check(r.htmlUrl == "https://github.com/JRDevo/VR-EyeCapture/releases/tag/v0.6.0", "release html_url (not the author's)");
        Check(r.assetName == "VR-EyeCapture-Setup-v0.6.0.exe", "the Setup asset is chosen (not the OBS zip listed first)");
        Check(r.assetUrl == "https://github.com/JRDevo/VR-EyeCapture/releases/download/v0.6.0/VR-EyeCapture-Setup-v0.6.0.exe", "asset browser_download_url");
        Check(r.digestHex == "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "asset digest sha256 hex extracted (prefix stripped)");
        Check(!r.draft && !r.prerelease, "not a draft / prerelease");
    }

    printf("=== release JSON parse: edge cases\n");
    {
        upd::ReleaseInfo r;
        Check(!upd::ParseLatestRelease(R"({"tag_name":"v1.0.0","draft":true,"assets":[{"name":"VR-EyeCapture-Setup-v1.0.0.exe","state":"uploaded","browser_download_url":"https://github.com/x.exe","digest":"sha256:aa"}]})",
                                       upd::kAssetGlob, r) && !r.ok && r.draft, "draft release -> not ok");
        Check(!upd::ParseLatestRelease(R"({"tag_name":"v1.0.0","prerelease":true,"draft":false,"assets":[]})", upd::kAssetGlob, r) && !r.ok && r.prerelease, "prerelease -> not ok");
        Check(!upd::ParseLatestRelease(R"({"tag_name":"v1.0.0","draft":false,"prerelease":false,"assets":[{"name":"something-else.zip","state":"uploaded","browser_download_url":"https://github.com/x.zip"}]})",
                                       upd::kAssetGlob, r) && !r.ok, "no matching asset -> not ok");
        // a release whose Setup asset has no digest: still ok (the DOWNLOAD step refuses an empty digest), digestHex empty
        Check(upd::ParseLatestRelease(R"({"tag_name":"v2.0.0","draft":false,"prerelease":false,"html_url":"https://github.com/r","assets":[{"name":"VR-EyeCapture-Setup-v2.0.0.exe","state":"uploaded","browser_download_url":"https://github.com/VR-EyeCapture-Setup-v2.0.0.exe"}]})",
                                      upd::kAssetGlob, r) && r.ok && r.digestHex.empty() && r.version.major == 2, "no digest -> still parses, digestHex empty");
        Check(!upd::ParseLatestRelease("not json at all", upd::kAssetGlob, r) && !r.ok, "garbage JSON -> not ok");
        // a still-uploading asset is skipped
        Check(!upd::ParseLatestRelease(R"({"tag_name":"v3.0.0","draft":false,"prerelease":false,"assets":[{"name":"VR-EyeCapture-Setup-v3.0.0.exe","state":"starter","browser_download_url":"https://github.com/x.exe"}]})",
                                       upd::kAssetGlob, r) && !r.ok, "asset still uploading (state != uploaded) -> not ok");
    }

    printf("=== update_test %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
