// update -- see update.h.
#include "update.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace upd {
namespace {

// ---- a tiny JSON parser (objects, arrays, strings with escapes, numbers, true/false/null). Enough for the GitHub
// "releases/latest" response; it never trusts field order and only reads the members we name. -------------------------
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool        b = false;
    double      num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Find(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string Str(const char* key) const { const JsonValue* v = Find(key); return v && v->type == Type::String ? v->str : std::string(); }
    bool        Bool(const char* key) const { const JsonValue* v = Find(key); return v && v->type == Type::Bool && v->b; }
};

struct JsonParser {
    const char* p;
    const char* end;
    bool ok = true;

    void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }

    void Utf8(unsigned cp, std::string& out) {
        if (cp <= 0x7F) out.push_back((char)cp);
        else if (cp <= 0x7FF) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    }
    unsigned Hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4 && p < end; ++i, ++p) {
            v <<= 4;
            const char c = *p;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else { ok = false; return 0; }
        }
        return v;
    }
    std::string String() {
        std::string s;
        if (p >= end || *p != '"') { ok = false; return s; }
        ++p;
        while (p < end) {
            const char c = *p++;
            if (c == '"') return s;
            if (c == '\\') {
                if (p >= end) { ok = false; return s; }
                const char e = *p++;
                switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    unsigned cp = Hex4();
                    if (!ok) return s;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < end && p[0] == '\\' && p[1] == 'u') {   // surrogate pair
                        p += 2;
                        const unsigned lo = Hex4();
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp <= 0xFFFF) Utf8(cp, s);
                    else {   // 4-byte UTF-8
                        s.push_back((char)(0xF0 | (cp >> 18))); s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: ok = false; return s;
                }
            } else {
                s.push_back(c);
            }
        }
        ok = false;
        return s;
    }
    JsonValue Value(int depth) {
        JsonValue v;
        if (depth > 64) { ok = false; return v; }   // runaway nesting guard
        Ws();
        if (p >= end) { ok = false; return v; }
        const char c = *p;
        if (c == '"') { v.type = JsonValue::Type::String; v.str = String(); }
        else if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++p; Ws();
            if (p < end && *p == '}') { ++p; return v; }
            while (p < end) {
                Ws();
                std::string key = String();
                if (!ok) return v;
                Ws();
                if (p >= end || *p != ':') { ok = false; return v; }
                ++p;
                JsonValue val = Value(depth + 1);
                if (!ok) return v;
                v.obj.emplace_back(std::move(key), std::move(val));
                Ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return v; }
                ok = false; return v;
            }
            ok = false;
        } else if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++p; Ws();
            if (p < end && *p == ']') { ++p; return v; }
            while (p < end) {
                JsonValue val = Value(depth + 1);
                if (!ok) return v;
                v.arr.push_back(std::move(val));
                Ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return v; }
                ok = false; return v;
            }
            ok = false;
        } else if (c == 't' && end - p >= 4 && memcmp(p, "true", 4) == 0) { v.type = JsonValue::Type::Bool; v.b = true; p += 4; }
        else if (c == 'f' && end - p >= 5 && memcmp(p, "false", 5) == 0) { v.type = JsonValue::Type::Bool; v.b = false; p += 5; }
        else if (c == 'n' && end - p >= 4 && memcmp(p, "null", 4) == 0) { v.type = JsonValue::Type::Null; p += 4; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* s = p;
            if (*p == '-') ++p;
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
            v.type = JsonValue::Type::Number;
            v.num = atof(std::string(s, p).c_str());
        } else {
            ok = false;
        }
        return v;
    }
};

bool ParseJson(const std::string& text, JsonValue& out) {
    JsonParser jp{text.data(), text.data() + text.size()};
    out = jp.Value(0);
    return jp.ok;   // trailing whitespace / bytes after the top-level value are ignored (GitHub sends one clean object)
}

std::string Lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

} // namespace

bool ParseVersion(const std::string& tag, Version& out) {
    size_t i = 0;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i < tag.size() && (tag[i] == 'v' || tag[i] == 'V')) ++i;
    int parts[3] = {0, 0, 0};
    int n = 0;
    while (n < 3) {
        if (i >= tag.size() || tag[i] < '0' || tag[i] > '9') return false;   // each of the three fields needs a digit
        long val = 0;
        while (i < tag.size() && tag[i] >= '0' && tag[i] <= '9') { val = val * 10 + (tag[i] - '0'); if (val > 1000000000) val = 1000000000; ++i; }
        parts[n++] = (int)val;
        if (n < 3) {
            if (i >= tag.size() || tag[i] != '.') return false;
            ++i;
        }
    }
    out.major = parts[0]; out.minor = parts[1]; out.patch = parts[2];
    return true;
}

int CompareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string VersionText(const Version& v) {
    char b[48];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%d.%d.%d", v.major, v.minor, v.patch);
    return b;
}

bool WildcardMatch(const std::string& textIn, const std::string& patternIn) {
    const std::string text = Lower(textIn), pattern = Lower(patternIn);
    size_t t = 0, p = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == text[t] || pattern[p] == '?')) { ++t; ++p; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool ParseLatestRelease(const std::string& json, const std::string& assetGlob, ReleaseInfo& out) {
    out = ReleaseInfo();
    JsonValue root;
    if (!ParseJson(json, root) || root.type != JsonValue::Type::Object) { out.error = "the release JSON could not be parsed"; return false; }
    out.draft = root.Bool("draft");
    out.prerelease = root.Bool("prerelease");
    out.tag = root.Str("tag_name");
    out.htmlUrl = root.Str("html_url");
    const bool haveVer = ParseVersion(out.tag, out.version);

    // the matching asset (first by name), with its digest (if any)
    const JsonValue* assets = root.Find("assets");
    if (assets && assets->type == JsonValue::Type::Array) {
        for (const JsonValue& a : assets->arr) {
            if (a.type != JsonValue::Type::Object) continue;
            const std::string name = a.Str("name");
            if (name.empty() || !WildcardMatch(name, assetGlob)) continue;
            const std::string state = a.Str("state");
            if (!state.empty() && state != "uploaded") continue;   // skip a still-uploading / failed asset
            out.assetName = name;
            out.assetUrl = a.Str("browser_download_url");
            std::string digest = a.Str("digest");                  // "sha256:<hex>"
            if (digest.compare(0, 7, "sha256:") == 0) out.digestHex = Lower(digest.substr(7));
            break;
        }
    }

    if (out.draft || out.prerelease) { out.error = "the latest release is a draft / prerelease"; return false; }
    if (out.tag.empty() || !haveVer) { out.error = "the release has no usable version tag"; return false; }
    if (out.assetName.empty() || out.assetUrl.empty()) { out.error = "no installer asset matching " + assetGlob + " in the release"; return false; }
    out.ok = true;
    return true;
}

bool IsAllowedDownloadHost(const std::wstring& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    std::wstring h(host);
    for (wchar_t& c : h) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    if (h == L"github.com" || h == L"objects.githubusercontent.com") return true;
    const std::wstring suf = L".githubusercontent.com";
    return h.size() > suf.size() && h.compare(h.size() - suf.size(), suf.size(), suf) == 0;
}

bool IsGithubUrl(const std::string& url) {
    const std::string pfx = "https://github.com/";
    if (url.size() < pfx.size()) return false;
    for (size_t i = 0; i < pfx.size(); ++i) {
        char c = url[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');   // scheme + host are case-insensitive
        if (c != pfx[i]) return false;
    }
    return true;
}

namespace {

// RAII for a WinHTTP handle.
struct WH {
    HINTERNET h = nullptr;
    WH() = default;
    explicit WH(HINTERNET x) : h(x) {}
    ~WH() { if (h) WinHttpCloseHandle(h); }
    WH(const WH&) = delete;
    WH& operator=(const WH&) = delete;
    operator HINTERNET() const { return h; }
};

std::wstring Win32Msg(DWORD e) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS |
                                   FORMAT_MESSAGE_FROM_HMODULE, GetModuleHandleW(L"winhttp.dll"), e, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = (n && buf) ? std::wstring(buf, n) : L"error " + std::to_wstring(e);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s;
}

// Opens a request to one URL (no auto-redirect). Returns the request handle (and keeps the session / connection alive in
// `sess` / `conn`), with the status code and, if present, the Location header. Caller reads the body or follows Location.
bool OpenRequest(const std::wstring& url, const std::wstring& userAgent, DWORD timeoutMs, WH& sess, WH& conn, WH& req,
                 int& status, std::wstring& location, std::wstring& err) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"";
    wchar_t path[4096] = L"";
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) { err = L"bad URL"; return false; }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    sess.h = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess.h) { err = Win32Msg(GetLastError()); return false; }
    WinHttpSetTimeouts(sess.h, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
    conn.h = WinHttpConnect(sess.h, host, uc.nPort, 0);
    if (!conn.h) { err = Win32Msg(GetLastError()); return false; }
    req.h = WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                               https ? WINHTTP_FLAG_SECURE : 0);
    if (!req.h) { err = Win32Msg(GetLastError()); return false; }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;   // we follow redirects ourselves, host-checked
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { err = Win32Msg(GetLastError()); return false; }
    if (!WinHttpReceiveResponse(req.h, nullptr)) { err = Win32Msg(GetLastError()); return false; }

    DWORD code = 0, len = sizeof(code);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        err = Win32Msg(GetLastError()); return false;
    }
    status = (int)code;
    location.clear();
    if (status >= 300 && status < 400) {
        DWORD ll = 0;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &ll, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && ll > 0) {
            std::wstring loc(ll / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, &loc[0], &ll, WINHTTP_NO_HEADER_INDEX)) {
                loc.resize(ll / sizeof(wchar_t));
                location = loc;
            }
        }
    }
    return true;
}

} // namespace

bool HttpGet(const std::wstring& url, const std::wstring& userAgent, std::string& body, std::wstring& err, DWORD timeoutMs, int* statusOut) {
    body.clear();
    std::wstring current = url;
    for (int hop = 0; hop < 10; ++hop) {
        WH sess, conn, req;
        int status = 0;
        // api.github.com is https and does not redirect; add the Accept header on the request before sending it
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        wchar_t host[256] = L"", path[4096] = L"";
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
        if (!WinHttpCrackUrl(current.c_str(), (DWORD)current.size(), 0, &uc)) { err = L"bad URL"; return false; }
        const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
        sess.h = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sess.h) { err = Win32Msg(GetLastError()); return false; }
        WinHttpSetTimeouts(sess.h, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
        conn.h = WinHttpConnect(sess.h, host, uc.nPort, 0);
        if (!conn.h) { err = Win32Msg(GetLastError()); return false; }
        req.h = WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (!req.h) { err = Win32Msg(GetLastError()); return false; }
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        static const wchar_t kAccept[] = L"Accept: application/vnd.github+json\r\n";
        WinHttpAddRequestHeaders(req.h, kAccept, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) { err = Win32Msg(GetLastError()); return false; }
        if (!WinHttpReceiveResponse(req.h, nullptr)) { err = Win32Msg(GetLastError()); return false; }
        DWORD code = 0, len = sizeof(code);
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) {
            err = Win32Msg(GetLastError()); return false;
        }
        status = (int)code;
        if (statusOut) *statusOut = status;
        if (status >= 300 && status < 400) {   // follow (https hosts only)
            DWORD ll = 0;
            WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &ll, WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && ll > 0) {
                std::wstring loc(ll / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, &loc[0], &ll, WINHTTP_NO_HEADER_INDEX)) {
                    loc.resize(ll / sizeof(wchar_t));
                    if (loc.compare(0, 6, L"https:") != 0) { err = L"refused a non-https redirect"; return false; }
                    current = loc;
                    continue;
                }
            }
            err = L"redirect without a usable Location"; return false;
        }
        if (status != 200) { err = L"HTTP status " + std::to_wstring(status); return false; }
        char buf[16384];
        DWORD got = 0;
        do {
            got = 0;
            if (!WinHttpReadData(req.h, buf, sizeof(buf), &got)) { err = Win32Msg(GetLastError()); return false; }
            if (got) body.append(buf, got);
            if (body.size() > 8u * 1024 * 1024) { err = L"response too large"; return false; }
        } while (got);
        return true;
    }
    err = L"too many redirects";
    return false;
}

bool DownloadFile(const std::wstring& url, const std::wstring& destPath, const std::wstring& userAgent, std::wstring& err,
                  std::function<void(unsigned long long, unsigned long long)> progress) {
    if (!IsAllowedDownloadHost(url)) { err = L"the download URL is not an allowed https host"; return false; }
    std::wstring current = url;
    for (int hop = 0; hop < 10; ++hop) {
        if (!IsAllowedDownloadHost(current)) { err = L"a redirect pointed at a host that is not allowed"; return false; }
        WH sess, conn, req;
        int status = 0;
        std::wstring location;
        if (!OpenRequest(current, userAgent, 30000, sess, conn, req, status, location, err)) return false;
        if (status >= 300 && status < 400) {
            if (location.empty()) { err = L"redirect without a usable Location"; return false; }
            current = location;
            continue;
        }
        if (status != 200) { err = L"HTTP status " + std::to_wstring(status); return false; }

        unsigned long long total = 0;
        {
            wchar_t cl[64] = L"";
            DWORD cll = sizeof(cl);
            if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cll, WINHTTP_NO_HEADER_INDEX))
                total = _wcstoui64(cl, nullptr, 10);
        }
        HANDLE f = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { err = L"cannot create " + destPath + L" (" + Win32Msg(GetLastError()) + L")"; return false; }
        char buf[65536];
        unsigned long long sofar = 0;
        bool good = true;
        if (progress) progress(0, total);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(req.h, buf, sizeof(buf), &got)) { err = Win32Msg(GetLastError()); good = false; break; }
            if (!got) break;
            DWORD wrote = 0;
            if (!WriteFile(f, buf, got, &wrote, nullptr) || wrote != got) { err = L"write error (disk full?)"; good = false; break; }
            sofar += got;
            if (sofar > 2ull * 1024 * 1024 * 1024) { err = L"download larger than 2 GB -- refused"; good = false; break; }
            if (progress) progress(sofar, total);
        }
        CloseHandle(f);
        if (!good) { DeleteFileW(destPath.c_str()); return false; }
        if (total && sofar != total) { DeleteFileW(destPath.c_str()); err = L"download truncated (" + std::to_wstring(sofar) + L" of " + std::to_wstring(total) + L" bytes)"; return false; }
        return true;
    }
    err = L"too many redirects";
    return false;
}

std::string Sha256Hex(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::string();

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    unsigned char digest[32];
    std::vector<unsigned char> buf(1 << 16);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        bool good = true;
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) { good = false; break; }
            if (!got) break;
            if (BCryptHashData(hash, buf.data(), got, 0) != 0) { good = false; break; }
        }
        if (good && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* hx = "0123456789abcdef";
            result.reserve(64);
            for (unsigned char c : digest) { result.push_back(hx[c >> 4]); result.push_back(hx[c & 0xF]); }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return result;
}

} // namespace upd
