// postprocess -- see postprocess.h.
#include "postprocess.h"
#include "disk.h"
#include <shlobj.h>
#include <cmath>
#include <cwctype>
#include "log.h"

namespace pp {
namespace {

std::wstring FileName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}
std::wstring StripExt(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    const size_t d = p.find_last_of(L'.');
    return (d == std::wstring::npos || (s != std::wstring::npos && d < s)) ? p : p.substr(0, d);
}
std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }
std::string  N(const std::wstring& s) { return elog::Narrow(s); }

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool FileSize(const std::wstring& p, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return true;
}

// First free name: `path` itself, else "<stem> (2).<ext>", "<stem> (3).<ext>" ... A conversion never overwrites a file.
std::wstring FreePath(const std::wstring& path) {
    if (!FileExists(path)) return path;
    const size_t s = path.find_last_of(L"\\/");
    const size_t d = path.find_last_of(L'.');
    const bool hasExt = d != std::wstring::npos && (s == std::wstring::npos || d > s);
    const std::wstring stem = hasExt ? path.substr(0, d) : path, ext = hasExt ? path.substr(d) : std::wstring();
    for (int i = 2; i < 10000; ++i) {
        const std::wstring cand = stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (!FileExists(cand)) return cand;
    }
    return path;
}

// First non-empty line of ffmpeg's output ("" if none).
std::wstring FirstLine(const std::string& out) {
    const std::wstring w = elog::Widen(out);
    size_t i = 0;
    while (i < w.size()) {
        size_t e = w.find_first_of(L"\r\n", i);
        if (e == std::wstring::npos) e = w.size();
        size_t a = i, b = e;
        while (a < b && iswspace(w[a])) ++a;
        while (b > a && iswspace(w[b - 1])) --b;
        if (b > a) return w.substr(a, b - a);
        i = e + 1;
    }
    return L"";
}

// Recursive search (depth-limited, no reparse points) for ffmpeg.exe below `dir`.
std::wstring FindFfmpegBelow(const std::wstring& dir, int depth) {
    const std::wstring direct = dir + L"\\ffmpeg.exe";
    if (FileExists(direct)) return direct;
    if (depth <= 0) return L"";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        found = FindFfmpegBelow(dir + L"\\" + fd.cFileName, depth - 1);
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// Value of "key":"value" in a flat one-line JSON object ("" if absent). Enough for the recorder's sidecar.
std::string JsonString(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = j.find('"', p + 1);
    if (p == std::string::npos) return "";
    const size_t e = j.find('"', p + 1);
    return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
}
// "key":<number> in one line (-1 when absent; spaces after the colon are allowed). The sidecar and queue.json.
long JsonInt(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k);
    if (p == std::string::npos) return -1;
    const char* s = j.c_str() + p + k.size();
    while (*s == ' ') ++s;
    char* end = nullptr;
    const long v = strtol(s, &end, 10);
    return end == s ? -1 : v;
}

// Value of a "key=value" line in ffprobe's `-of default=nw=1` output ("" if absent).
std::string ProbeValue(const std::string& out, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t p = 0;
    while ((p = out.find(k, p)) != std::string::npos) {
        if (p == 0 || out[p - 1] == '\n' || out[p - 1] == '\r') {
            const size_t s = p + k.size();
            const size_t e = out.find_first_of("\r\n", s);
            return out.substr(s, e == std::string::npos ? std::string::npos : e - s);
        }
        p += k.size();
    }
    return "";
}

// "66/1", "60000/1001" or "30" -> frames per second. False for "0/0" and garbage.
bool ParseRate(const std::string& s, double& fps) {
    double num = 0.0, den = 1.0;
    const size_t slash = s.find('/');
    char* end = nullptr;
    num = strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    if (slash != std::string::npos) {
        const char* d = s.c_str() + slash + 1;
        den = strtod(d, &end);
        if (end == d) return false;
    }
    if (!(num > 0.0) || !(den > 0.0)) return false;
    fps = num / den;
    return true;
}

// The per-eye 3D layout filter graph shared by the MKV and the 8K step (flat side-by-side input only).
// Both eyes use the same expression on the same input size, so their geometry is identical. No padding.
//   lay       the layout (layout_3d)
//   shortSide 0 = NATIVE size (no upscale: sc = min(1, 8192/long side of the frame)); else the FRAME's short side S
//             (sc = min(S/short side, 8192/long side))
//   fpsCap    "" or "fps=60," (goes first, so the scalers only see the kept frames)
// Eye units: the cropped half of the SBS input is iw x ih. The frame is fw x fh in eye units: iw x ih for the half layouts
// (each eye squeezed to half height / width), 2*iw x ih for sbs_full, iw x 2*ih for tb_full. Stored eye size:
// 2*round(iw*sc/(2 or 4)) x 2*round(ih*sc/(2 or 4)), 4 in the squeezed direction (all dimensions even).
struct LayoutGraph {
    std::wstring vf;               // -vf value (without quotes)
    const wchar_t* stereoMode;     // Matroska stereo_mode: left_right / top_bottom
    const char* name;              // layout_3d value
    const char* rule;              // human-readable description for the log
};
// eye_crop: when not None and the eye (iw/2 x ih of the SBS input) is taller than the target aspect (16:9 or 4:3), each eye is
// centre-cropped vertically to that aspect BEFORE scaling: height = 2*trunc(eyeW*D/N/2) (even), y = even centre offset; both eyes use
// the same numbers. An eye that is already wider than the target is left alone (min(ih, ...)).
// EyeAspect gives the target as N:D (0 = no crop). 4:3pad: after the 4:3 crop, a pad filter adds black bars left and right in
// eye units, BEFORE the scale (whose expression then sees the padded eye as iw): width max(iw, 2*round(ih*16/9/2)), picture at
// x = 2*floor((ow-iw)/4) -- pp::PredictLayout's pw / padX. Never stretches.
// The log line uses the same arithmetic (pp::PredictLayout, postprocess.h, also behind the Settings preview).
LayoutGraph BuildLayoutGraph(Layout3d lay, EyeCrop crop, int shortSide, const std::wstring& fpsCap) {
    LayoutGraph g;
    const bool sbs = lay == Layout3d::SbsHalf || lay == Layout3d::SbsFull;
    g.stereoMode = sbs ? L"left_right" : L"top_bottom";
    g.name = lay == Layout3d::SbsHalf ? "sbs_half" : lay == Layout3d::SbsFull ? "sbs_full"
             : lay == Layout3d::TbFull ? "tb_full" : "tb_half";
    g.rule = lay == Layout3d::TbHalf ? "frame = one eye's aspect, each eye squeezed to half height, left eye on top"
             : lay == Layout3d::SbsHalf ? "frame = one eye's aspect, each eye squeezed to half width, left eye left"
             : lay == Layout3d::SbsFull ? "frame = 2x one eye's aspect (side by side), each eye at its own aspect"
                                        : "frame = one eye's aspect / 2 (top-bottom), each eye at its own aspect";
    const std::wstring fw = lay == Layout3d::SbsFull ? L"2*iw" : L"iw";
    const std::wstring fh = lay == Layout3d::TbFull ? L"2*ih" : L"ih";
    const std::wstring longCap = L"8192/max(" + fw + L"\\," + fh + L")";
    const std::wstring sc = shortSide > 0
        ? L"min(" + std::to_wstring(shortSide) + L"/min(" + fw + L"\\," + fh + L")\\," + longCap + L")"
        : L"min(1\\," + longCap + L")";
    const wchar_t* wDiv = lay == Layout3d::SbsHalf ? L"4" : L"2";   // sbs_half: eye width halved
    const wchar_t* hDiv = lay == Layout3d::TbHalf ? L"4" : L"2";    // tb_half: eye height halved
    const std::wstring fit = L"scale=w=2*round(iw*" + sc + L"/" + wDiv + L"):h=2*round(ih*" + sc + L"/" + hDiv + L"):flags=lanczos";
    int cn, cd;
    EyeAspect(crop, cn, cd);
    const std::wstring cropH = cn > 0 ? L"min(ih\\,2*trunc(iw/2*" + std::to_wstring(cd) + L"/" + std::to_wstring(cn) + L"/2))" : L"ih";
    const std::wstring cropY = cn > 0 ? L"trunc((ih-oh)/4)*2" : L"0";
    // 4:3pad: the bars (iw x ih here = the cropped eye; ow = the padded width)
    const std::wstring pad = crop == EyeCrop::Pad43 ? L",pad=w=max(iw\\,2*trunc(ih*16/9/2+0.5)):h=ih:x=trunc((ow-iw)/4)*2:y=0:color=black" : L"";
    const std::wstring cl = L"crop=iw/2:" + cropH + L":0:" + cropY + pad, cr = L"crop=iw/2:" + cropH + L":iw/2:" + cropY + pad;
    g.vf = fpsCap + L"split=2[l][r];[l]" + cl + L"," + fit + L"[L];[r]" + cr + L"," + fit +
           std::wstring(L"[R];[L][R]") + (sbs ? L"hstack" : L"vstack") + L"=inputs=2,setsar=1,format=yuv420p";
    return g;
}

bool EndsWithI(const std::wstring& s, const wchar_t* tail) {
    const size_t k = wcslen(tail);
    return s.size() >= k && _wcsicmp(s.c_str() + s.size() - k, tail) == 0;
}

// Deletes stale partial outputs of `mp4` left by an interrupted conversion: files in its folder named <stem> + '.', '_' or
// ' ' + ... ending in ".mkv.part" / ".mp4.part" (the only .part names a step writes). Only called while no step of this
// tray writes one (Start() before the worker runs; a job's start), so every such file is garbage.
void DeleteStaleParts(const std::wstring& mp4, const char* when) {
    const size_t slash = mp4.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"." : mp4.substr(0, slash);
    const std::wstring base = FileName(StripExt(mp4));
    if (base.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + base + L"*.part").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring n = fd.cFileName;
        if (n.size() <= base.size() || _wcsnicmp(n.c_str(), base.c_str(), base.size()) != 0) continue;
        const wchar_t c = n[base.size()];
        if (c != L'.' && c != L'_' && c != L' ') continue;
        if (!EndsWithI(n, L".mkv.part") && !EndsWithI(n, L".mp4.part")) continue;
        const std::wstring full = dir + L"\\" + n;
        if (DeleteFileW(full.c_str())) LOGI("job: deleted stale partial output %s (left by an interrupted conversion; %s)", N(n).c_str(), when);
        else LOGW("job: could not delete stale partial output %s (error %lu)", N(n).c_str(), GetLastError());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// NtSuspendProcess / NtResumeProcess (ntdll, undocumented but stable since XP): suspend / resume every thread of a process.
using NtProcFn = LONG(NTAPI*)(HANDLE);
NtProcFn NtProc(const char* name) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? reinterpret_cast<NtProcFn>(reinterpret_cast<void*>(GetProcAddress(nt, name))) : nullptr;
}

} // namespace

Recording ReadSidecar(const std::wstring& mp4) {
    Recording r;
    r.sidecar = StripExt(mp4) + L".json";
    HANDLE h = CreateFileW(r.sidecar.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;   // no sidecar: flat SBS (also every recording made before dome mode)
    char buf[1024];
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok) return r;
    const std::string j(buf, got);
    // only the recorder's sidecar counts (it always has "projection"): an unrelated <name>.json is neither read nor deleted
    if (j.find("\"projection\"") == std::string::npos) return r;
    const std::string proj = JsonString(j, "projection"), layout = JsonString(j, "layout");
    r.hasSidecar = true;
    if (proj == "vr180") r.projection = Recording::Vr180;
    else if (proj == "vr360") r.projection = Recording::Vr360;
    // record_for_youtube: a flat recording already in the YouTube layout ("youtube":1). The packing key names the layout;
    // without it, "layout" says top_bottom (tb_half, the default) or left_right (sbs_half).
    if (r.projection == Recording::Flat && JsonInt(j, "youtube") == 1) {
        r.packed = true;
        const std::string pk = JsonString(j, "packing"), ec = JsonString(j, "eye_crop");
        r.packing = pk == "sbs_half" ? Layout3d::SbsHalf : pk == "sbs_full" ? Layout3d::SbsFull : pk == "tb_full" ? Layout3d::TbFull
                    : pk == "tb_half" ? Layout3d::TbHalf : layout == "left_right" ? Layout3d::SbsHalf : Layout3d::TbHalf;
        r.crop = CropFromName(ec);
        const long w = JsonInt(j, "width"), h = JsonInt(j, "height");
        r.width = w > 0 ? (int)w : 0;
        r.height = h > 0 ? (int)h : 0;
    }
    const bool tbPacking = r.packing == Layout3d::TbHalf || r.packing == Layout3d::TbFull;
    r.topBottom = layout == "top_bottom" || (layout.empty() && (r.projection == Recording::Vr360 || (r.packed && tbPacking)));
    return r;
}

const wchar_t* Recording::Name() const {
    if (packed) return packing == Layout3d::SbsHalf ? L"YouTube sbs_half" : packing == Layout3d::SbsFull ? L"YouTube sbs_full"
                       : packing == Layout3d::TbFull ? L"YouTube tb_full" : L"YouTube tb_half";
    return projection == Vr180 ? L"VR180" : projection == Vr360 ? L"VR360" : L"3D SBS";
}

std::wstring ResolveFfmpeg(const Config& cfg, const std::wstring& exeDir, std::wstring& how) {
    if (!cfg.ffmpegPath.empty()) {
        if (FileExists(cfg.ffmpegPath)) { how = L"ffmpeg_path from config.ini"; return cfg.ffmpegPath; }
        how = L"ffmpeg_path from config.ini does not exist: " + cfg.ffmpegPath;
        return L"";
    }
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) { how = L"PATH"; return buf; }
    wchar_t la[MAX_PATH];
    const DWORD m = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (m > 0 && m < MAX_PATH) {
        const std::wstring r = FindFfmpegBelow(std::wstring(la) + L"\\Microsoft\\WinGet\\Packages", 6);
        if (!r.empty()) { how = L"WinGet packages"; return r; }
    }
    const std::wstring local = exeDir + L"\\ffmpeg\\ffmpeg.exe";
    if (FileExists(local)) { how = L"ffmpeg folder next to the tray app"; return local; }
    how = L"not on PATH, not in WinGet packages, not in " + exeDir + L"\\ffmpeg";
    return L"";
}

// ------------------------------------------------------------------------------------------------------------------

namespace {
// CreateProcessW (suspended, no window, below normal) where the child inherits ONLY its own std handles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST):
// the tag thread and the conversion worker launch children concurrently, and a plain bInheritHandles would leak one child's pipe write
// end into the other (the reader then gets no EOF until that other child exits).
BOOL CreateChildOwnHandles(std::wstring& cl, HANDLE in, HANDLE out, HANDLE err, PROCESS_INFORMATION& pi) {
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in;
    si.StartupInfo.hStdOutput = out;
    si.StartupInfo.hStdError = err;
    HANDLE list[3];
    DWORD n = 0;
    for (HANDLE h : {in, out, err}) {
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        bool dup = false;
        for (DWORD i = 0; i < n; ++i) if (list[i] == h) dup = true;
        if (!dup) list[n++] = h;
    }
    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    std::vector<char> mem(sz);
    LPPROC_THREAD_ATTRIBUTE_LIST al = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(mem.data());
    bool haveList = false;
    if (n && InitializeProcThreadAttributeList(al, 1, 0, &sz)) {
        haveList = UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, n * sizeof(HANDLE), nullptr, nullptr) != FALSE;
        if (!haveList) DeleteProcThreadAttributeList(al);
    }
    DWORD flags = CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED;
    if (haveList) {
        si.lpAttributeList = al;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    const BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD e = GetLastError();
    if (haveList) DeleteProcThreadAttributeList(al);
    SetLastError(e);
    return ok;
}

// Runs a command line to completion, stdout + stderr merged into `output`. The child sits in its own kill-on-close job object, so a
// tray that dies takes it along. False = could not start (output says why).
bool RunTool(const std::wstring& cmdline, std::string& output, DWORD& exitCode) {
    output.clear();
    exitCode = (DWORD)-1;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { output = "could not create a pipe"; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    const BOOL ok = CreateChildOwnHandles(cl, nul, wr, wr, pi);
    const DWORD createErr = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        // the tool's own name (first token of the command line, quoted or not) so a failing ffprobe is not reported as ffmpeg
        std::wstring exe = cmdline.empty() ? std::wstring() : cmdline[0] == L'"' ? cmdline.substr(1, cmdline.find(L'"', 1) - 1) : cmdline.substr(0, cmdline.find(L' '));
        const size_t sl = exe.find_last_of(L"\\/");
        if (sl != std::wstring::npos) exe = exe.substr(sl + 1);
        char msg[128];
        snprintf(msg, sizeof(msg), "could not start %s (error %lu)", exe.empty() ? "the tool" : N(exe).c_str(), (unsigned long)createErr);
        output = msg;
        return false;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(job, pi.hProcess);   // failure: the child just runs as it would without the job
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
        if (output.size() < 65536) output.append(buf, got);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    if (job) CloseHandle(job);
    return true;
}
} // namespace

bool Tag3d(const Config& cfg, const std::wstring& exeDir, const std::wstring& in, bool topBottom, std::wstring& outPath, std::wstring& why) {
    outPath.clear();
    why.clear();
    const std::string inN = N(in);
    const char* mode = topBottom ? "top_bottom" : "left_right";
    if (!FileExists(in)) {
        why = L"file not found";
        LOGE("tag3d: %s: file not found", inN.c_str());
        return false;
    }
    std::wstring how;
    const std::wstring ff = ResolveFfmpeg(cfg, exeDir, how);
    if (ff.empty()) {
        why = L"ffmpeg not found — run: winget install Gyan.FFmpeg.Essentials";
        LOGE("tag3d: %s: ffmpeg not found (%s)", inN.c_str(), N(how).c_str());
        return false;
    }
    const std::wstring finalPath = FreePath(StripExt(in) + L"_3D.mkv");
    const std::wstring part = finalPath + L".part";
    if (finalPath != StripExt(in) + L"_3D.mkv") LOGI("tag3d: %s exists -- writing %s instead", N(StripExt(in) + L"_3D.mkv").c_str(), N(FileName(finalPath)).c_str());
    // -dn: data streams (a mov's timecode track ...) cannot go into Matroska; every other stream is copied.
    const std::wstring cmd = Q(ff) + L" -hide_banner -loglevel error -y -i " + Q(in) + L" -map 0 -dn -c copy -metadata:s:v:0 stereo_mode=" +
                             (topBottom ? L"top_bottom" : L"left_right") + L" -f matroska " + Q(part);
    LOGI("tag3d: %s -> %s (stereo_mode=%s): %s", inN.c_str(), N(FileName(finalPath)).c_str(), mode, N(cmd).c_str());
    std::string out;
    DWORD code = 0;
    auto fail = [&](const std::wstring& reason) {
        DeleteFileW(part.c_str());
        why = reason;
        LOGE("tag3d: %s FAILED: %s", inN.c_str(), N(reason).c_str());
        return false;
    };
    if (!RunTool(cmd, out, code)) return fail(elog::Widen(out));
    if (code != 0) {
        std::wstring r = FirstLine(out);
        if (r.empty()) r = L"ffmpeg exit code " + std::to_wstring(code);
        return fail(r);
    }
    unsigned long long size = 0;
    if (!FileSize(part, size) || size == 0) return fail(L"ffmpeg wrote no output");
    // ffprobe sits next to ffmpeg: the remux must be as long as the source (within 1 s / 2%)
    const size_t slash = ff.find_last_of(L"\\/");
    const std::wstring probe = (slash == std::wstring::npos ? std::wstring() : ff.substr(0, slash + 1)) + L"ffprobe.exe";
    if (FileExists(probe)) {
        auto duration = [&](const std::wstring& file, double& sec) {
            std::string po;
            DWORD pc = 0;
            if (!RunTool(Q(probe) + L" -v error -show_entries format=duration -of default=nw=1 " + Q(file), po, pc) || pc != 0) return false;
            const std::string v = ProbeValue(po, "duration");
            char* end = nullptr;
            sec = strtod(v.c_str(), &end);
            return !v.empty() && end != v.c_str() && sec > 0.0;
        };
        double a = 0.0, b = 0.0;
        if (duration(in, a) && duration(part, b)) {
            const double tol = a * 0.02 > 1.0 ? a * 0.02 : 1.0;
            if (std::fabs(a - b) > tol) {
                char msg[96];
                snprintf(msg, sizeof(msg), "output is %.1f s, the source %.1f s", b, a);
                return fail(elog::Widen(msg));
            }
            LOGI("tag3d: verified %.2f s vs source %.2f s, %llu bytes", b, a, size);
        } else {
            LOGW("tag3d: ffprobe could not read the durations -- not checked");
        }
    } else {
        LOGW("tag3d: ffprobe.exe not next to ffmpeg -- duration not checked");
    }
    const std::wstring dest = FreePath(finalPath);   // a file that appeared meanwhile is never overwritten
    if (!MoveFileExW(part.c_str(), dest.c_str(), 0)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "could not rename the output (error %lu)", GetLastError());
        return fail(elog::Widen(msg));
    }
    outPath = dest;
    LOGI("tag3d: done %s (%llu bytes)", N(dest).c_str(), size);
    return true;
}

void Processor::Start(const std::wstring& exeDir, HWND wnd, UINT msg) {
    exeDir_ = exeDir;
    wnd_ = wnd;
    msg_ = msg;
    // every child (ffmpeg / ffprobe) goes into this job object: when the tray dies (crash, kill, logoff) the kernel closes the
    // handle and kills them, also a hold-suspended ffmpeg
    if (!jobObj_) {
        jobObj_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!jobObj_ || !SetInformationJobObject(jobObj_, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
            LOGW("job: no job object for ffmpeg (error %lu) -- an ffmpeg would outlive a crashed tray", GetLastError());
            if (jobObj_) CloseHandle(jobObj_);
            jobObj_ = nullptr;
        }
    }
    LoadQueue();
    thread_ = std::thread([this] { Worker(); });
}

// ---- queue.json: {"version":1,"jobs":[ one object per line ]}. Paths are UTF-8 with '\' and '"' escaped (a Windows path
// has no '"', so the reader's "find the next quote" is exact).
namespace {
std::string JsonEsc(const std::wstring& w) {
    std::string s;
    for (char ch : elog::Narrow(w)) {
        if (ch == '\\' || ch == '"') s += '\\';
        s += ch;
    }
    return s;
}
std::wstring JsonUnesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        o += s[i];
    }
    return elog::Widen(o);
}
std::string JobLine(const std::wstring& path, bool own, const JobFormat& f, const std::wstring& doneMkv, const std::wstring& doneFlat,
                    const std::wstring& doneYt8k) {
    std::string s = "{\"path\":\"" + JsonEsc(path) + "\",\"own_format\":" + (own ? "1" : "0");
    // finished steps of the running job (file names next to the MP4): a restored job skips them
    if (!doneMkv.empty()) s += ",\"done_mkv\":\"" + JsonEsc(doneMkv) + "\"";
    if (!doneFlat.empty()) s += ",\"done_flat\":\"" + JsonEsc(doneFlat) + "\"";
    if (!doneYt8k.empty()) s += ",\"done_yt8k\":\"" + JsonEsc(doneYt8k) + "\"";
    if (own) {
        s += ",\"post_mkv\":" + std::to_string((int)f.postMkv) + ",\"post_flat\":" + std::to_string((int)f.postFlat) +
             ",\"post_yt8k\":" + std::to_string((int)f.postYt8k) + ",\"keep_mp4\":" + std::to_string((int)f.keepMp4) +
             ",\"layout_3d\":\"" + elog::Narrow(LayoutName(f.layout3d)) + "\",\"eye_crop\":\"" + elog::Narrow(CropName(f.eyeCrop)) +
             "\",\"yt8k_codec\":\"" + (f.yt8kCodec == VideoCodec::Av1 ? "av1" : "hevc") + "\",\"yt8k_bitrate_mbps\":" +
             std::to_string(f.yt8kBitrateMbps);
    }
    return s + "}";
}
} // namespace

void Processor::SaveQueue() {
    if (exeDir_.empty()) return;
    std::lock_guard<std::mutex> sk(saveM_);
    std::string out = "{\"version\":1,\"jobs\":[\n";
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto add = [&](const Job& j) {
            out += (n++ ? ",\n" : "") + JobLine(j.path, j.ownFormat, j.fmt, j.doneMkv, j.doneFlat, j.doneYt8k);
        };
        if (busy_ && !runningJob_.path.empty()) add(runningJob_);
        for (const auto& q : queue_) add(q);
    }
    out += "\n]}\n";
    const std::wstring path = exeDir_ + L"\\queue.json", tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { LOGW("queue: cannot write %s (error %lu)", N(tmp).c_str(), GetLastError()); return; }
    DWORD put = 0;
    // flushed before the rename: a power loss right after it must not leave an empty queue.json
    const BOOL ok = WriteFile(h, out.data(), (DWORD)out.size(), &put, nullptr) && put == out.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOGW("queue: cannot replace %s (error %lu)", N(path).c_str(), GetLastError());
        DeleteFileW(tmp.c_str());
    }
}

void Processor::LoadQueue() {
    const std::wstring path = exeDir_ + L"\\queue.json";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got && text.size() < 4 * 1024 * 1024) text.append(buf, got);
    CloseHandle(h);
    size_t restored = 0, dropped = 0, pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        if (line.find("\"path\":") == std::string::npos) continue;
        // JsonString stops at the first quote after the value's opening quote: paths have their quotes escaped
        size_t p = line.find("\"path\":\"");
        if (p == std::string::npos) continue;
        p += 8;
        size_t q = p;
        while (q < line.size() && line[q] != '"') q += line[q] == '\\' ? 2 : 1;
        if (q >= line.size()) continue;
        Job j;
        j.path = JsonUnesc(line.substr(p, q - p));
        j.ownFormat = JsonInt(line, "own_format") == 1;
        if (j.ownFormat) {
            j.fmt.postMkv = JsonInt(line, "post_mkv") == 1;
            j.fmt.postFlat = JsonInt(line, "post_flat") == 1;
            j.fmt.postYt8k = JsonInt(line, "post_yt8k") == 1;
            j.fmt.keepMp4 = JsonInt(line, "keep_mp4") == 1;
            const std::string lay = JsonString(line, "layout_3d"), crop = JsonString(line, "eye_crop");
            j.fmt.layout3d = lay == "sbs_half" ? Layout3d::SbsHalf : lay == "sbs_full" ? Layout3d::SbsFull : lay == "tb_full" ? Layout3d::TbFull : Layout3d::TbHalf;
            j.fmt.eyeCrop = CropFromName(crop);
            j.fmt.yt8kCodec = JsonString(line, "yt8k_codec") == "av1" ? VideoCodec::Av1 : VideoCodec::Hevc;
            const long br = JsonInt(line, "yt8k_bitrate_mbps");
            j.fmt.yt8kBitrateMbps = br >= 20 && br <= 400 ? (unsigned)br : 150;
        }
        j.doneMkv = JsonUnesc(JsonString(line, "done_mkv"));
        j.doneFlat = JsonUnesc(JsonString(line, "done_flat"));
        j.doneYt8k = JsonUnesc(JsonString(line, "done_yt8k"));
        DeleteStaleParts(j.path, "queue.json restored");   // no step runs yet: every .part of it is from an interrupted run
        if (!FileExists(j.path)) {
            ++dropped;
            LOGI("queue: queue.json: %s no longer exists -- dropped", N(j.path).c_str());
            continue;
        }
        j.userHeld = true;   // after a restart nothing starts on its own
        j.id = nextId_++;
        std::string doneTxt;
        for (const std::wstring* d : {&j.doneMkv, &j.doneFlat, &j.doneYt8k})
            if (!d->empty()) doneTxt += (doneTxt.empty() ? "; already made: " : ", ") + N(*d);
        LOGI("queue: restored %s (%s%s) -- waits for you (Start / Start all)", N(FileName(j.path)).c_str(),
             j.ownFormat ? N(FormatText(j.fmt)).c_str() : "settings of config.ini at its start", doneTxt.c_str());
        queue_.push_back(j);
        lastQueued_ = j.path;
        ++restored;
    }
    if (restored || dropped) LOGI("queue: restored %zu job(s) from queue.json (%zu dropped: MP4 missing)", restored, dropped);
    if (dropped) SaveQueue();
}

void Processor::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
        userPause_ = false;
        if (childSuspended_) SuspendChildLocked(false, "tray exiting");   // resume first: never leave a suspended ffmpeg behind
        if (child_) TerminateProcess(child_, 1);   // the job notices quit_, deletes its partial output and returns
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (jobObj_) { CloseHandle(jobObj_); jobObj_ = nullptr; }   // no child is left (the worker waited for it); kill-on-close otherwise
}

bool Processor::Enqueue(const std::wstring& mp4, bool force, const JobFormat* fmt, bool userHeld) {
    // a job without a snapshot counts as config.ini's current values (that is what it would convert with if it started now)
    Config cfg;
    LoadConfig(exeDir_, cfg);
    const JobFormat cur = FormatFromConfig(cfg);
    const JobFormat want = fmt ? *fmt : cur;
    if (force) userHeld = false;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (quit_) return false;
        if (busy_ && _wcsicmp(running_.c_str(), mp4.c_str()) == 0 && runningFmt_ == want) {
            if (force && !runningForced_) {   // "Convert last recording now" on the running (maybe paused) job: never hold it again
                runningForced_ = true;
                ApplySuspendLocked("forced");   // stays suspended only when the user paused it
                LOGI("queue: %s is already running -- now forced (ignores the hold)", N(FileName(mp4)).c_str());
                cv_.notify_all();
                return true;
            }
            LOGI("queue: %s (%s) is already running", N(FileName(mp4)).c_str(), N(FormatText(want)).c_str());
            return false;
        }
        for (auto& q : queue_) {
            if (_wcsicmp(q.path.c_str(), mp4.c_str()) != 0 || (q.ownFormat ? q.fmt : cur) != want) continue;
            if (force && !q.force) {   // "Convert last recording now" / Start on a held job: it runs next
                q.force = true;
                q.forceSeq = nextForceSeq_++;
                q.userHeld = false;
                LOGI("queue: %s was waiting for the hold -- forced, runs now", N(FileName(mp4)).c_str());
                cv_.notify_all();
                queued = true;
                break;
            }
            LOGI("queue: %s (%s) is already waiting", N(FileName(mp4)).c_str(), N(FormatText(want)).c_str());
            return false;
        }
        if (!queued) {
            Job j;
            j.id = nextId_++;
            j.path = mp4;
            j.force = force;
            j.forceSeq = force ? nextForceSeq_++ : 0;
            j.userHeld = userHeld;
            j.ownFormat = fmt != nullptr;
            j.fmt = fmt ? *fmt : JobFormat();
            queue_.push_back(j);
            lastQueued_ = mp4;
            if (userHeld)
                LOGI("queue: %s added (%s) -- waits for you (Start / Start all)", N(FileName(mp4)).c_str(), N(FormatText(want)).c_str());
            else if (hold_ && !force)
                LOGI("queue: %s queued while held -- converts %s", N(FileName(mp4)).c_str(), N(holdReason_).c_str());
            if (fmt && !userHeld) LOGI("queue: %s format: %s", N(FileName(mp4)).c_str(), N(FormatText(want)).c_str());
            cv_.notify_all();
        }
    }
    SaveQueue();
    Poke();
    return true;
}

bool Processor::StartJob(unsigned long long id) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (quit_ || !id) return false;
        if (busy_ && runningJob_.id == id) {
            if (!runningForced_) {
                runningForced_ = true;
                ApplySuspendLocked("forced");
                LOGI("queue: %s is already running -- now forced (ignores the hold)", N(FileName(running_)).c_str());
            }
        } else {
            auto it = queue_.begin();
            for (; it != queue_.end(); ++it) if (it->id == id) break;
            if (it == queue_.end()) return false;
            if (!it->force) {
                it->force = true;
                it->forceSeq = nextForceSeq_++;
                it->userHeld = false;
                LOGI("queue: %s started by user -- runs next (ignores post_when)", N(FileName(it->path)).c_str());
            }
        }
        cv_.notify_all();
    }
    SaveQueue();
    Poke();
    return true;
}

size_t Processor::StartAll() {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& q : queue_) {
            if (q.force) continue;
            q.force = true;
            q.forceSeq = nextForceSeq_++;
            q.userHeld = false;
            ++n;
        }
        if (n) LOGI("queue: start all by user -- %zu job(s) run one after another, oldest first (ignore post_when)", n);
        cv_.notify_all();
    }
    SaveQueue();
    Poke();
    return n;
}

std::deque<Processor::Job>::iterator Processor::PickLocked() {
    // the FORCED job forced first (Start / Convert now: "next"); else, when not held, the oldest job not held by the user
    auto best = queue_.end();
    for (auto it = queue_.begin(); it != queue_.end(); ++it)
        if (it->force && (best == queue_.end() || it->forceSeq < best->forceSeq)) best = it;
    if (best != queue_.end()) return best;
    if (!hold_)
        for (auto it = queue_.begin(); it != queue_.end(); ++it) if (!it->userHeld) return it;
    return queue_.end();
}

bool Processor::StartableLocked() const {
    for (const auto& q : queue_) if (q.force || (!hold_ && !q.userHeld)) return true;
    return false;
}

void Processor::SuspendChildLocked(bool suspend, const char* why) {
    if (!child_ || childSuspended_ == suspend) return;
    static const NtProcFn s_suspend = NtProc("NtSuspendProcess");
    static const NtProcFn s_resume = NtProc("NtResumeProcess");
    const NtProcFn fn = suspend ? s_suspend : s_resume;
    const LONG status = fn ? fn(child_) : (LONG)0xC0000002L;   // STATUS_NOT_IMPLEMENTED
    if (status >= 0) {
        childSuspended_ = suspend;
        const ULONGLONG now = GetTickCount64();
        if (suspend) suspendStart_ = now;
        else suspendedMs_ += now - suspendStart_;   // the ETA ignores suspended time
        LOGI("job: ffmpeg (pid %lu) %s (%s)", GetProcessId(child_), suspend ? "SUSPENDED" : "resumed", why);
    } else {
        LOGW("job: %s ffmpeg (pid %lu) failed (NTSTATUS 0x%08lX)", suspend ? "suspending" : "resuming", GetProcessId(child_),
             (unsigned long)status);
    }
}

void Processor::SetHold(bool hold, const std::wstring& reason) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const std::wstring r = hold ? reason : std::wstring();
        changed = hold != hold_ || r != holdReason_;
        if (!changed) return;
        if (hold != hold_)
            LOGI("queue: hold %s%s%s (%zu queued, %s)", hold ? "ON -- conversions wait, converts " : "OFF",
                 hold ? N(r).c_str() : "", hold ? "" : " -- conversions may run", queue_.size(),
                 busy_ ? (runningForced_ ? "a forced job is running" : "a job is running") : "no job running");
        hold_ = hold;
        holdReason_ = r;
        // a running non-forced job: suspend its ffmpeg while held, resume it when released (unless the user paused it)
        if (busy_ && !runningForced_) ApplySuspendLocked(hold ? "hold" : "hold cleared");
    }
    cv_.notify_all();
    Poke();
}

bool Processor::TogglePause(bool& paused) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!busy_ || cancel_ || quit_) return false;
        userPause_ = !userPause_;
        paused = userPause_;
        const std::string held = !paused && HoldPausedLocked() ? "; still held -- converts " + N(holdReason_) : std::string();
        LOGI("job: %s by user (%s, step %s%s)", paused ? "PAUSED" : "resumed", N(st_.file).c_str(), N(st_.step).c_str(), held.c_str());
        ApplySuspendLocked(paused ? "paused by user" : "resumed by user");
    }
    cv_.notify_all();
    Poke();
    return true;
}

bool Processor::Cancel() {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!busy_ || quit_) return false;
        if (cancel_) return true;
        cancel_ = true;
        userPause_ = false;
        LOGI("job: cancel requested by user (%s, step %s)", N(st_.file).c_str(), N(st_.step).c_str());
        if (childSuspended_) SuspendChildLocked(false, "cancel");   // resume first: never leave a suspended ffmpeg behind
        if (child_) TerminateProcess(child_, 1);   // RunJob notices cancel_, deletes the partial output, skips the rest
    }
    cv_.notify_all();
    Poke();
    return true;
}

double Processor::ActiveSecLocked() const {
    if (!child_) return 0.0;
    const ULONGLONG now = GetTickCount64();
    ULONGLONG susp = suspendedMs_ + (childSuspended_ ? now - suspendStart_ : 0);
    const ULONGLONG wall = now - childStart_;
    return wall > susp ? (wall - susp) / 1000.0 : 0.0;
}

// One progress block from ffmpeg (doneSec = out_time). Updates percent / ETA, pokes the UI at most once per second
// (and at the end), logs each 25% mark once.
void Processor::OnProgress(double doneSec, double durSec, bool end) {
    if (durSec <= 0.0) return;
    if (doneSec < 0.0) doneSec = 0.0;
    double frac = doneSec / durSec;
    if (frac > 1.0) frac = 1.0;
    if (end) frac = 1.0;
    bool poke = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        const double active = ActiveSecLocked();
        st_.percent = (int)(frac * 100.0);
        if (end) st_.etaSec = 0;
        else if (active >= 1.5 && doneSec > 0.0) st_.etaSec = (int)((durSec - doneSec) * active / doneSec + 0.5);
        const ULONGLONG now = GetTickCount64();
        if (end || now - lastProgressPoke_ >= 1000) { lastProgressPoke_ = now; poke = true; }
        const int quarter = (int)(frac * 4.0);
        if (quarter > lastLoggedQuarter_ && quarter < 4) {
            lastLoggedQuarter_ = quarter;
            LOGI("job: %s progress %d%% (%.1f of %.1f s, %.1f s active%s)", N(st_.step).c_str(), quarter * 25, doneSec, durSec, active,
                 st_.etaSec >= 0 ? (", about " + std::to_string(st_.etaSec) + " s left").c_str() : "");
        }
    }
    if (poke) Poke();
}

State Processor::GetState() {
    std::lock_guard<std::mutex> lk(m_);
    State s = st_;
    s.queued = queue_.size();
    s.userHeld = 0;
    for (const auto& q : queue_) if (q.userHeld) ++s.userHeld;
    s.path = running_;
    s.runningId = busy_ ? runningJob_.id : 0;
    s.format = busy_ ? FormatText(runningFmt_) : std::wstring();
    s.forced = busy_ && runningForced_;
    s.held = hold_;
    s.holdReason = holdReason_;
    s.paused = PausedLocked();
    s.userPaused = busy_ && userPause_;
    s.cancelling = busy_ && cancel_;
    return s;
}

std::vector<QueuedJob> Processor::Queued() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<QueuedJob> v;
    v.reserve(queue_.size());
    for (const auto& q : queue_) {
        QueuedJob j;
        j.id = q.id;
        j.path = q.path;
        j.force = q.force;
        j.forceSeq = q.forceSeq;
        j.userHeld = q.userHeld;
        j.ownFormat = q.ownFormat;
        j.fmt = q.fmt;
        v.push_back(j);
    }
    return v;
}

bool Processor::Remove(unsigned long long id) {
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = queue_.begin();
        for (; it != queue_.end(); ++it) if (it->id == id) break;
        if (it == queue_.end()) return false;
        LOGI("queue: %s removed by user (%zu left waiting; the MP4 stays)", N(FileName(it->path)).c_str(), queue_.size() - 1);
        queue_.erase(it);
    }
    SaveQueue();
    Poke();
    return true;
}

bool Processor::PopNotice(Notice& n) {
    std::lock_guard<std::mutex> lk(m_);
    if (notices_.empty()) return false;
    n = notices_.front();
    notices_.pop_front();
    return true;
}

std::wstring Processor::LastQueued() {
    std::lock_guard<std::mutex> lk(m_);
    return lastQueued_;
}

void Processor::Poke() {
    if (wnd_) PostMessageW(wnd_, msg_, 0, 0);
}

void Processor::SetStep(const wchar_t* step) {
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.step = step;
        st_.percent = -1;
        st_.etaSec = -1;
    }
    Poke();
}

void Processor::Finish(bool ok, const std::wstring& lastLine, const Notice* notice) {
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.lastLine = lastLine;
        st_.lastOk = ok;
        if (notice) notices_.push_back(*notice);
    }
    Poke();
}

// A step's output is verified and has its final name: remember it in the running job and persist it (queue.json), so a job
// interrupted later (Exit, logoff, a kill) does not redo this step after a restart. The first finished step also freezes the
// effective format as the job's snapshot: the restored job must make the remaining steps with the same settings.
void Processor::StepDone(const char* step, const std::wstring& file) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!busy_) return;
        std::wstring& slot = !strcmp(step, "MKV") ? runningJob_.doneMkv : !strcmp(step, "flat") ? runningJob_.doneFlat : runningJob_.doneYt8k;
        slot = FileName(file);
        if (!runningJob_.ownFormat) {
            runningJob_.ownFormat = true;
            runningJob_.fmt = runningFmt_;
        }
    }
    SaveQueue();
}

void Processor::Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return quit_ || StartableLocked(); });
            if (quit_) break;
            // the first-forced FORCED job; else (not held) the oldest job not held by the user. The others keep waiting, in order.
            auto it = PickLocked();
            if (it == queue_.end()) continue;   // cannot happen: StartableLocked guarantees one
            job = *it;
            runningForced_ = it->force;
            queue_.erase(it);
            runningJob_ = job;
            running_ = job.path;
            runningFmt_ = job.fmt;   // RunJob replaces it with the effective format once it has read config.ini
            busy_ = true;
            userPause_ = false;
            cancel_ = false;
            st_.percent = -1;
            st_.etaSec = -1;
            if (runningForced_ && hold_) LOGI("job: %s is forced -- runs although conversions are held", N(FileName(job.path)).c_str());
            st_.busy = true;
            st_.file = FileName(job.path);
            st_.step = L"MKV";
        }
        SaveQueue();
        Poke();
        const bool interrupted = RunJob(job);
        {
            std::lock_guard<std::mutex> lk(m_);
            running_.clear();
            runningJob_ = Job();
            busy_ = false;
            runningForced_ = false;
            userPause_ = false;
            cancel_ = false;
            st_.percent = -1;
            st_.etaSec = -1;
            st_.busy = false;
            st_.file.clear();
            st_.step.clear();
        }
        // interrupted by Stop() (tray exiting before the job completed): queue.json keeps the job as last written (running, with
        // its finished steps), so it comes back after a restart. A job that completed / failed / was cancelled leaves it, also
        // when Exit came during its non-abortable tail.
        if (!interrupted) SaveQueue();
        Poke();
    }
}

// Runs a command line with stdout+stderr merged and captured, at below-normal priority so a game keeps its CPU.
bool Processor::RunFfmpeg(const std::wstring& cmdline, std::string& output, DWORD& exitCode, bool progress, double durSec) {
    output.clear();
    exitCode = (DWORD)-1;
    {
        // paused (hold for a non-forced job, or the user's pause): do not start the next ffmpeg until it clears
        std::unique_lock<std::mutex> lk(m_);
        if (PausedLocked() && !quit_ && !cancel_) {
            const bool user = userPause_;
            LOGI("job: %s -- the next ffmpeg step waits until %s", user ? "paused by user" : "held",
                 user ? "it is resumed" : "the hold clears");
            cv_.wait(lk, [this] { return quit_ || cancel_ || !PausedLocked(); });
            if (!quit_ && !cancel_) LOGI("job: %s -- continuing", user ? "resumed" : "hold cleared");
        }
        if (quit_) { output = "cancelled (tray exiting)"; return false; }
        if (cancel_) { output = "cancelled by user"; return false; }
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr, erd = nullptr, ewr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (progress) {   // stderr on its own pipe: stdout carries the progress blocks
        if (!CreatePipe(&erd, &ewr, &sa, 0)) { CloseHandle(rd); CloseHandle(wr); return false; }
        SetHandleInformation(erd, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    // suspended until it is in the job object (kill on close: it never outlives the tray), then resumed
    const BOOL ok = CreateChildOwnHandles(cl, nul, wr, progress ? ewr : wr, pi);
    const DWORD createErr = GetLastError();
    CloseHandle(wr);
    if (ewr) CloseHandle(ewr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (erd) CloseHandle(erd);
        char msg[64];
        snprintf(msg, sizeof(msg), "could not start ffmpeg (error %lu)", (unsigned long)createErr);
        output = msg;
        return false;
    }
    if (jobObj_ && !AssignProcessToJobObject(jobObj_, pi.hProcess) && !assignWarned_) {
        assignWarned_ = true;   // e.g. the tray runs in a job that forbids nesting: the child runs as before
        LOGW("job: could not put ffmpeg in the tray's job object (error %lu) -- it would outlive a crashed tray (logged once)", GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(m_);
        child_ = pi.hProcess;
        childSuspended_ = false;
        childStart_ = GetTickCount64();
        suspendedMs_ = 0;
        if (progress) {
            st_.percent = durSec > 0.0 ? 0 : -1;
            st_.etaSec = -1;
            lastLoggedQuarter_ = 0;
            lastProgressPoke_ = 0;
        }
        if (quit_ || cancel_) TerminateProcess(pi.hProcess, 1);
        else if (PausedLocked()) SuspendChildLocked(true, userPause_ ? "paused by user" : "hold");   // paused between the wait and CreateProcess
    }
    if (progress) Poke();
    char buf[4096];
    DWORD got = 0;
    if (!progress) {
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
            if (output.size() < 65536) output.append(buf, got);
        }
    } else {
        // stderr (errors) drained by a helper thread into `output`; stdout = "key=value" lines, a block per update
        // ending with "progress=continue" / "progress=end".
        std::string errText;
        std::thread errReader([erd, &errText] {
            char eb[4096];
            DWORD n = 0;
            while (ReadFile(erd, eb, sizeof(eb), &n, nullptr) && n) {
                if (errText.size() < 65536) errText.append(eb, n);
            }
        });
        std::string line;
        double done = -1.0;
        while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) {
            for (DWORD i = 0; i < got; ++i) {
                const char ch = buf[i];
                if (ch != '\n') { if (ch != '\r' && line.size() < 256) line += ch; continue; }
                if (line.compare(0, 12, "out_time_us=") == 0) {
                    char* end = nullptr;
                    const long long us = strtoll(line.c_str() + 12, &end, 10);
                    if (end != line.c_str() + 12) done = us / 1e6;   // "N/A" before the first frame: kept at -1
                } else if (line.compare(0, 9, "progress=") == 0) {
                    OnProgress(done, durSec, line.compare(9, std::string::npos, "end") == 0);
                }
                line.clear();
            }
        }
        errReader.join();
        output = errText;
    }
    if (erd) CloseHandle(erd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    {
        std::lock_guard<std::mutex> lk(m_);
        child_ = nullptr;
        childSuspended_ = false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return true;
}

bool Processor::RunJob(const Job& job) {
    const std::wstring& mp4 = job.path;
    const ULONGLONG t0 = GetTickCount64();
    const std::wstring name = FileName(mp4);
    bool interrupted = false;   // stopped() while the tray exits: the job did not complete and stays in queue.json
    // tray exiting (Stop) or the user cancelled this job (Cancel): the running ffmpeg was terminated
    auto aborted = [this] { std::lock_guard<std::mutex> lk(m_); return quit_ || cancel_; };
    // After aborted(): deletes the step's partial output (`partial` = its .part, "" = none), never the MP4; a user cancel
    // ends the job with a notice (not a failure, keep_mp4 not applied); a tray exit marks the job interrupted.
    auto stopped = [&](const std::wstring& partial) {
        bool user = false;
        std::wstring step;
        {
            std::lock_guard<std::mutex> lk(m_);
            user = cancel_ && !quit_;
            step = st_.step;
        }
        std::string del = "nothing to delete";
        if (!partial.empty() && FileExists(partial)) {
            del = DeleteFileW(partial.c_str()) ? "deleted partial " + N(FileName(partial))
                                               : "could not delete partial " + N(FileName(partial)) + " (error " + std::to_string(GetLastError()) + ")";
        }
        if (!user) {
            interrupted = true;
            LOGW("job: cancelled (tray exiting) during the %s step: %s", N(step).c_str(), del.c_str());
            return;
        }
        LOGI("job: cancelled by user %s during the %s step (%s; MP4 kept, remaining steps skipped)", N(name).c_str(), N(step).c_str(),
             del.c_str());
        Notice n{L"VR EyeCapture", L"Conversion cancelled: " + name + L"\nThe MP4 is kept.", false};
        Finish(true, L"last: " + name + L" conversion cancelled", &n);
    };
    auto fail = [&](const std::wstring& why, bool balloon = true) {
        LOGE("job: FAILED %s: %s (MP4 left untouched)", N(name).c_str(), N(why).c_str());
        Notice n{L"Conversion failed", name + L"\n" + why, true};
        Finish(false, L"last: " + name + L" conversion FAILED (see tray.log)", balloon ? &n : nullptr);
    };

    Config cfg;
    LoadConfig(exeDir_, cfg);
    if (job.ownFormat) ApplyFormat(job.fmt, cfg);   // the job's snapshot; every other key from config.ini
    {
        std::lock_guard<std::mutex> lk(m_);
        runningFmt_ = FormatFromConfig(cfg);
    }
    LOGI("job: start %s (post_mkv=%d post_flat=%d post_yt8k=%d keep_mp4=%d) format %s (%s)", N(name).c_str(), cfg.postMkv, cfg.postFlat,
         cfg.postYt8k, cfg.keepMp4, N(FormatText(FormatFromConfig(cfg))).c_str(),
         job.ownFormat ? "the job's own settings" : "config.ini at job start");
    if (!cfg.postMkv && !cfg.postFlat && !cfg.postYt8k) {
        LOGI("job: post_mkv, post_flat and post_yt8k are all 0 -- nothing to do");
        Notice n{L"Nothing to convert", job.ownFormat ? L"This job's settings create no files (YouTube output None, no 2D video)."
                                                      : L"post_mkv, post_flat and post_yt8k are all 0 in config.ini.", false};
        Finish(true, L"", &n);
        return false;
    }

    std::wstring how;
    const std::wstring ff = ResolveFfmpeg(cfg, exeDir_, how);
    if (ff.empty()) {
        LOGE("job: ffmpeg not found (%s)", N(how).c_str());
        Notice n{L"ffmpeg not found", L"ffmpeg not found — run: winget install Gyan.FFmpeg.Essentials", true};
        Finish(false, L"last: " + name + L" not converted (ffmpeg not found)", &n);
        return false;
    }
    LOGI("job: ffmpeg = %s (%s)", N(ff).c_str(), N(how).c_str());
    // half-written outputs of an interrupted earlier run of this MP4 (no step of ours writes one now)
    DeleteStaleParts(mp4, "job start");

    // The layer has just closed the file; give the filesystem a moment if it is still locked.
    for (int i = 0;; ++i) {
        HANDLE h = CreateFileW(mp4.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); break; }
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) { fail(L"source MP4 no longer exists"); return false; }
        if (aborted()) { stopped(L""); return interrupted; }
        if (i >= 40) { fail(L"source MP4 is locked"); return false; }
        Sleep(250);
    }
    unsigned long long mp4Size = 0;
    FileSize(mp4, mp4Size);

    const Recording rec = ReadSidecar(mp4);
    LOGI("job: %s -> %s (%s, Matroska stereo_mode=%s)", rec.hasSidecar ? N(FileName(rec.sidecar)).c_str() : "no sidecar",
         N(rec.Name()).c_str(), rec.Dome() ? "projection side data from the MP4's sv3d box is carried over"
                                : rec.packed ? "flat, recorded in the YouTube layout (record_for_youtube)" : "flat side by side",
         rec.topBottom ? "top_bottom" : "left_right");
    // "tb_half, 16:9, 7680x4320" for the log lines of a packed recording (record_for_youtube)
    std::string packedDesc;
    if (rec.packed) {
        packedDesc = N(LayoutName(rec.packing)) + ", " + N(CropName(rec.crop));
        if (rec.width > 0 && rec.height > 0) packedDesc += ", " + std::to_string(rec.width) + "x" + std::to_string(rec.height);
    }
    const std::wstring base = StripExt(mp4);
    const size_t dirEnd = mp4.find_last_of(L"\\/");
    const std::wstring dir = dirEnd == std::wstring::npos ? std::wstring() : mp4.substr(0, dirEnd + 1);   // with its separator

    // Steps finished in an earlier run of this job (restored from queue.json, Processor::StepDone): skipped while their file
    // still exists (they count as ok for keep_mp4 and name that file in the balloon); made again when it is gone.
    auto doneFile = [&](const char* step, bool wanted, const std::wstring& file) {
        if (!wanted || file.empty()) return std::wstring();
        const std::wstring full = dir + file;
        if (FileExists(full)) {
            LOGI("job: %s step already done before the restart: %s (kept, not made again)", step, N(file).c_str());
            return full;
        }
        LOGW("job: %s step was done before the restart, but %s is gone -- making it again", step, N(file).c_str());
        return std::wstring();
    };
    const std::wstring prevMkv = doneFile("MKV", cfg.postMkv, job.doneMkv);
    const std::wstring prevFlat = doneFile("flat", cfg.postFlat, job.doneFlat);
    // dome and packed (record_for_youtube) recordings: no 8K step, the MKV step is always the plain remux (layout_3d /
    // eye_crop of this job do not apply: the dome has a fixed layout, a packed recording has them baked in)
    const bool fixedLayout = rec.Dome() || rec.packed;
    const std::wstring prevYt8k = doneFile("8K", cfg.postYt8k && !fixedLayout, job.doneYt8k);
    const bool runMkv = cfg.postMkv && prevMkv.empty(), runFlat = cfg.postFlat && prevFlat.empty();
    const bool runYt8k = cfg.postYt8k && !fixedLayout && prevYt8k.empty();
    const bool mkvReencode = !fixedLayout && (cfg.layout3d != Layout3d::SbsFull || cfg.eyeCrop != EyeCrop::None);

    // ffprobe sits next to ffmpeg in every ffmpeg build ("" = not found: durations / frame rate are then not checked).
    std::wstring probe;
    {
        const size_t slash = ff.find_last_of(L"\\/");
        const std::wstring cand = (slash == std::wstring::npos ? std::wstring() : ff.substr(0, slash + 1)) + L"ffprobe.exe";
        if (FileExists(cand)) probe = cand;
    }
    // Container duration in seconds (false when ffprobe is missing or cannot read the file).
    auto probeDuration = [&](const std::wstring& file, double& sec) {
        if (probe.empty()) return false;
        std::string po;
        DWORD pc = 0;
        if (!RunFfmpeg(Q(probe) + L" -v error -show_entries format=duration -of default=nw=1 " + Q(file), po, pc) || pc != 0) return false;
        const std::string v = ProbeValue(po, "duration");
        char* end = nullptr;
        sec = strtod(v.c_str(), &end);
        return !v.empty() && end != v.c_str() && sec > 0.0;
    };
    // The source duration: the 100% mark of every step's progress (out_time_us), and the output size estimate. 0 = unknown.
    double srcDur = 0.0;
    if (!probeDuration(mp4, srcDur)) {
        srcDur = 0.0;
        if (aborted()) { stopped(L""); return interrupted; }
        LOGW("job: source duration unknown (%s) -- no progress percent", probe.empty() ? "ffprobe.exe not next to ffmpeg" : "ffprobe failed");
    }

    // Low-space guard (src/disk.h): every output goes next to the MP4, so that drive must have room for the outputs this job
    // still makes (duration x the steps' video bitrates; a remux = the MP4's size; without a duration a re-encode counts as
    // the MP4's size) plus MinFreeToStart() to spare, so a long 8K job cannot fill the drive mid-way or push a live recording
    // into its stop. The MP4 is kept.
    {
        bool guessed = false;
        auto bytesAt = [&](double mbps) {
            if (srcDur > 0.0) return srcDur * mbps * 1e6 / 8.0;
            guessed = true;
            return (double)mp4Size;
        };
        double est = 0.0;
        if (runMkv) est += mkvReencode ? bytesAt(150.0) : (double)mp4Size;
        if (runYt8k) est += bytesAt((double)cfg.yt8kBitrateMbps);
        if (runFlat) est += bytesAt(40.0);
        const unsigned long long estB = (unsigned long long)est, need = disk::MinFreeToStart() + estB;
        unsigned long long freeB = 0;
        const bool known = disk::FreeBytes(mp4, freeB);
        const std::string estNote = guessed ? " (source duration unknown: re-encodes estimated at the MP4's size)" : "";
        if (known && freeB < need) {
            const std::wstring d = disk::DriveName(mp4);
            const std::wstring why = (d.empty() ? std::wstring(L"The drive") : d) + L" has only " + disk::SizeText(freeB) +
                                     L" free; this conversion needs about " + disk::SizeText(need) + L" (about " + disk::SizeText(estB) +
                                     L" of output + " + disk::SizeText(disk::MinFreeToStart()) + L" to spare).";
            LOGE("job: NOT STARTED %s: low disk space -- %s%s (MP4 kept)", N(name).c_str(), N(why).c_str(), estNote.c_str());
            Notice n{L"Not enough disk space", name + L" was not converted. " + why + L" The MP4 is kept.", true};
            Finish(false, L"last: " + name + L" not converted (disk almost full)", &n);
            return false;
        }
        LOGI("job: disk: %s, needs about %s (about %s of output + %s to spare)%s", known ? N(disk::SizeText(freeB) + L" free").c_str() : "free space unknown",
             N(disk::SizeText(need)).c_str(), N(disk::SizeText(estB)).c_str(), N(disk::SizeText(disk::MinFreeToStart())).c_str(), estNote.c_str());
    }

    // Names. A step whose picture depends on eye_crop gets a tag when the crop is not the default 16:9 ("_4x3", "_4x3pad", "_full"), so
    // converting the same recording with two crops keeps both files. The MKV step is tagged only when it re-encodes (a plain
    // remux ignores eye_crop); _FLAT.mp4 is always 16:9 (no tag). A name that exists is never overwritten: " (2)", " (3)" ...
    // Each step writes <name>.part and renames it to <name> (FreePath'd again) once verified (commit).
    const wchar_t* cropTag = cfg.eyeCrop == EyeCrop::Std43 ? L"_4x3" : cfg.eyeCrop == EyeCrop::Pad43 ? L"_4x3pad"
                             : cfg.eyeCrop == EyeCrop::None ? L"_full" : L"";
    auto pickName = [&](const char* step, const std::wstring& plain, bool wanted) {
        if (!wanted) return plain;
        const std::wstring chosen = FreePath(plain);
        if (chosen != plain)
            LOGI("job: %s output %s exists -- writing %s instead", step, N(FileName(plain)).c_str(), N(FileName(chosen)).c_str());
        return chosen;
    };
    std::wstring mkv = runMkv ? pickName("MKV", base + (mkvReencode ? cropTag : L"") + L".mkv", true) : prevMkv;
    std::wstring flat = runFlat ? pickName("flat", base + L"_FLAT.mp4", true) : prevFlat;
    // 8K: "_AV1" when the job asks for AV1 (renamed to the untagged name below if HEVC had to make it)
    const bool av1Tag = cfg.yt8kCodec == VideoCodec::Av1;
    const std::wstring yt8kPlain = base + L"_YT8K" + cropTag + L".mkv";
    std::wstring yt8k = runYt8k ? pickName("8K", av1Tag ? base + L"_YT8K" + cropTag + L"_AV1.mkv" : yt8kPlain, true) : prevYt8k;
    const std::wstring mkvPart = mkv + L".part", flatPart = flat + L".part", yt8kPart = yt8k + L".part";
    // A verified .part gets its final name `want` (FreePath'd again: a file that appeared meanwhile is never overwritten).
    // False = the rename failed (the .part is deleted; the caller fails the step).
    auto commit = [&](const char* step, const std::wstring& part, const std::wstring& want, std::wstring& dest) {
        const std::wstring to = FreePath(want);
        if (to != want)
            LOGI("job: %s output %s appeared meanwhile -- writing %s instead", step, N(FileName(want)).c_str(), N(FileName(to)).c_str());
        if (!MoveFileExW(part.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
            LOGE("job: %s step: could not rename %s to %s (error %lu)", step, N(FileName(part)).c_str(), N(FileName(to)).c_str(), GetLastError());
            DeleteFileW(part.c_str());
            return false;
        }
        dest = to;
        return true;
    };
    std::string out;
    DWORD code = 0;
    bool mkvOk = !prevMkv.empty(), flatOk = !prevFlat.empty(), yt8kOk = !prevYt8k.empty();
    // Every conversion ffmpeg reports progress on stdout (RunFfmpeg parses it); goes right after "-loglevel error".
    const std::wstring prog = L" -nostats -progress pipe:1";

    // NVENC detection (hevc_nvenc, av1_nvenc), shared by the MKV, flat and 8K steps: ffmpeg's encoder list is asked once
    // per job, only when a re-encode step runs. A listed encoder can still fail to open (no NVIDIA GPU, or no AV1 NVENC on
    // an older one); encode() then moves on to the next encoder.
    int nvencState = -1;   // -1 = not asked yet; bit 0 = hevc_nvenc, bit 1 = av1_nvenc
    auto nvencBits = [&]() {
        if (nvencState < 0) {
            std::string encOut;
            DWORD encCode = 0;
            const bool listed = RunFfmpeg(Q(ff) + L" -hide_banner -encoders", encOut, encCode) && encCode == 0;
            int bits = 0;
            if (listed && encOut.find("hevc_nvenc") != std::string::npos) bits |= 1;
            if (listed && encOut.find("av1_nvenc") != std::string::npos) bits |= 2;
            wchar_t noNv[8];
            if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_NO_NVENC", noNv, 8) > 0) {   // test-only: never touch the GPU
                LOGI("job: EYECAP_TRAY_TEST_NO_NVENC is set -- hevc_nvenc / av1_nvenc not used (test)");
                bits = 0;
            }
            nvencState = bits;
        }
        return nvencState;
    };
    // The source codec. An AV1 recording (codec = av1) is decoded on the GPU for the GPU encodes: ffmpeg's software AV1
    // decoder in the usual builds is libaom (no libdav1d in the Gyan essentials build), measured ~27 fps on an 8192x4348
    // file vs ~65 fps with -hwaccel d3d11va (frames come back to system memory, so the CPU filter graphs are unchanged).
    // The software fallback encodes keep the software decoder (a machine without a working NVENC may not decode AV1 on the
    // GPU either). Without ffprobe the codec is unknown and nothing changes.
    std::wstring hwDec;
    if (!probe.empty()) {
        std::string po;
        DWORD pc = 0;
        if (RunFfmpeg(Q(probe) + L" -v error -select_streams v:0 -show_entries stream=codec_name -of default=nw=1 " + Q(mp4), po, pc) && pc == 0) {
            const std::string cn = ProbeValue(po, "codec_name");
            if (cn == "av1") {
                hwDec = L" -hwaccel d3d11va";
                LOGI("job: source video is AV1 -- NVENC encodes decode it on the GPU (-hwaccel d3d11va); software encodes use ffmpeg's AV1 decoder");
            } else if (!cn.empty()) {
                LOGI("job: source video is %s", cn.c_str());
            }
        }
        if (aborted()) { stopped(L""); return interrupted; }
    }
    // Start of every re-encode command, up to and including "-i " (GPU attempts get hwDec in front of the input).
    auto head = [&](bool gpu) { return Q(ff) + L" -hide_banner -loglevel error" + prog + L" -y" + (gpu ? hwDec : std::wstring()) + L" -i "; };
    // One re-encode: tries `head(gpu) + input + args + tail` for each encoder in order until one works. An NVENC encoder
    // that ffmpeg does not list (or EYECAP_TRAY_TEST_NO_NVENC) is skipped; `note` (optional) is added to its "not available"
    // / "failed" line. `swWarn` (optional) is logged as a warning before a software encoder runs. Afterwards `ran` /
    // `code` / `out` describe the last attempt. Returns false when the tray is exiting (`outFile` deleted, nothing else to do).
    struct EncTry { const char* name; std::wstring args; int nvBit; const char* note; };   // nvBit 0 = software encoder
    const char* usedEnc = "";   // the encoder of the last successful encode()
    auto encode = [&](const char* step, const std::wstring& input, const std::vector<EncTry>& tries, const char* swWarn,
                      const std::wstring& tail, const std::wstring& outFile, bool& ran) {
        ran = false;
        code = (DWORD)-1;
        for (size_t i = 0; i < tries.size(); ++i) {
            const EncTry& t = tries[i];
            const char* next = i + 1 < tries.size() ? tries[i + 1].name : "nothing";
            const char* note = t.note ? t.note : "";
            if (t.nvBit && !(nvencBits() & t.nvBit)) {
                LOGI("job: %s not available -- using %s%s", t.name, next, note);
                continue;
            }
            if (!t.nvBit && swWarn) LOGW("job: %s", swWarn);
            const std::wstring cmd = head(t.nvBit != 0) + input + t.args + tail;
            LOGI("job: %s cmd (%s): %s", step, t.name, N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code, true, srcDur);
            if (aborted()) { stopped(outFile); return false; }
            if (ran && code == 0) { usedEnc = t.name; return true; }
            if (i + 1 < tries.size()) LOGW("job: %s failed (%s) -- retrying with %s%s", t.name, N(FirstLine(out)).c_str(), next, note);
        }
        return true;
    };

    // Logs the crop and the frame size the graph will produce (needs ffprobe; the graph itself does not depend on it).
    auto logFrame = [&](const char* step, const LayoutGraph& g, int shortSide) {
        const std::string cn = N(CropName(cfg.eyeCrop));
        std::string po;
        DWORD pc = 0;
        int iw = 0, ih = 0;
        if (!probe.empty() && RunFfmpeg(Q(probe) + L" -v error -select_streams v:0 -show_entries stream=width,height -of default=nw=1 " + Q(mp4), po, pc) &&
            pc == 0) {
            iw = atoi(ProbeValue(po, "width").c_str());
            ih = atoi(ProbeValue(po, "height").c_str());
        }
        if (iw < 4 || ih < 4) {
            LOGI("job: %s: eye_crop %s (frame size not logged: input size unknown)", step, cn.c_str());
            return;
        }
        const LayoutSize p = PredictLayout(iw / 2, ih, cfg.layout3d, cfg.eyeCrop, shortSide);
        const bool cropped = p.ch < ih;
        char padText[160] = "";
        if (p.pw > p.cw)   // 4:3pad: "black bars to 5462x3072 (picture 5760 wide at x 960 of each 7680 eye)"
            snprintf(padText, sizeof(padText), ", black bars left and right to %dx%d (picture %d wide at x %d of each %d-wide eye)",
                     p.pw, p.ch, p.vw, p.vx, p.sw);
        LOGI("job: %s: input %dx%d = eyes %dx%d; eye_crop %s -> %s%dx%d%s; %s frame %dx%d", step, iw, ih, iw / 2, ih, cn.c_str(),
             cropped ? "centre-cropped to " : "no crop needed, eyes ", p.cw, p.ch, padText, g.name, p.fw, p.fh);
    };

    // MKV step. layout_3d sbs_full (and every dome recording) = plain remux, nothing re-encoded: stream copy, Matroska
    // stereo_mode tag, verified by size (> 90% of the MP4). Any other layout = re-encode with the same per-eye layout filter
    // graph as the 8K step at NATIVE size (no upscale, long side <= 8192), source frame rate kept, hevc_nvenc (else libx265),
    // audio copied; verified by existence, size > 0 and duration (ffprobe) against the MP4.
    if (runMkv) {
        SetStep(L"MKV");
        const Layout3d lay = cfg.layout3d;
        const bool reencode = mkvReencode;
        const ULONGLONG s0 = GetTickCount64();
        unsigned long long mkvSize = 0;
        bool have = false, ran = false;
        if (!reencode) {
            if (rec.Dome())
                LOGI("job: MKV step: %s recording -- layout_3d does not apply; plain remux (-c copy), stereo_mode=%s", N(rec.Name()).c_str(),
                     rec.topBottom ? "top_bottom" : "left_right");
            else if (rec.packed)
                LOGI("job: MKV step: recorded in the YouTube layout (%s): remux only (-c copy), stereo_mode=%s; this job's layout_3d %s / "
                     "eye_crop %s do not apply", packedDesc.c_str(), rec.topBottom ? "top_bottom" : "left_right", N(LayoutName(cfg.layout3d)).c_str(),
                     N(CropName(cfg.eyeCrop)).c_str());
            else
                LOGI("job: MKV step: layout_3d sbs_full + eye_crop none -- plain remux (-c copy) of the full side-by-side recording, stereo_mode=left_right");
            const std::wstring cmd = Q(ff) + L" -hide_banner -loglevel error" + prog + L" -y -i " + Q(mp4) +
                                     L" -map 0 -c copy -metadata:s:v:0 stereo_mode=" + (rec.topBottom ? L"top_bottom " : L"left_right ") +
                                     L"-f matroska " + Q(mkvPart);
            LOGI("job: MKV cmd: %s", N(cmd).c_str());
            ran = RunFfmpeg(cmd, out, code, true, srcDur);
            if (aborted()) { stopped(mkvPart); return interrupted; }
        } else {
            const LayoutGraph g = BuildLayoutGraph(lay, cfg.eyeCrop, 0, L"");
            LOGI("job: MKV step: layout_3d %s: %s; native size (no upscale, long side <= 8192), source frame rate kept, re-encode",
                 g.name, g.rule);
            logFrame("MKV step", g, 0);
            const std::wstring input = Q(mp4) + L" -map 0:v:0 -map 0:a? -vf \"" + g.vf + L"\" ";
            const std::wstring tail = std::wstring(L" -c:a copy -max_muxing_queue_size 4096 -metadata:s:v:0 stereo_mode=") + g.stereoMode +
                                      L" -f matroska " + Q(mkvPart);
            const std::vector<EncTry> tries = {
                {"hevc_nvenc", L"-c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v 150M -maxrate 200M -bufsize 300M", 1, nullptr},
                {"libx265", L"-c:v libx265 -preset medium -crf 18", 0, nullptr}};
            if (!encode("MKV", input, tries, "MKV step: no working hevc_nvenc -- libx265 at this size is slow (many times the clip length)",
                        tail, mkvPart, ran))
                return interrupted;
        }
        have = FileSize(mkvPart, mkvSize);
        const double sec = (GetTickCount64() - s0) / 1000.0;
        if (!ran || code != 0) {
            DeleteFileW(mkvPart.c_str());
            std::wstring why = FirstLine(out);
            if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
            LOGE("job: MKV step FAILED after %.1f s: %s", sec, N(why).c_str());
            fail(why);
            return false;
        }
        if (!reencode) {
            if (!have || mkvSize * 10 <= mp4Size * 9) {
                DeleteFileW(mkvPart.c_str());
                LOGE("job: MKV step: output %s is %llu bytes vs MP4 %llu bytes (< 90%%)", have ? "too small" : "missing", mkvSize, mp4Size);
                fail(L"the MKV is missing or smaller than 90% of the MP4");
                return false;
            }
            LOGI("job: MKV verification (remux): exists, %llu bytes = %.1f%% of the MP4's %llu bytes (> 90%%) -- ok", mkvSize,
                 mp4Size ? 100.0 * (double)mkvSize / (double)mp4Size : 0.0, mp4Size);
        } else {
            if (!have || mkvSize == 0) {
                DeleteFileW(mkvPart.c_str());
                LOGE("job: MKV step: re-encoded output %s", have ? "is empty" : "is missing");
                fail(L"the re-encoded MKV is missing or empty");
                return false;
            }
            if (probe.empty()) {
                LOGW("job: MKV verification (re-encode): exists, %llu bytes > 0; ffprobe not found next to ffmpeg -- duration not checked", mkvSize);
            } else {
                double dMp4 = 0.0, dMkv = 0.0;
                const bool okMp4 = probeDuration(mp4, dMp4), okMkv = probeDuration(mkvPart, dMkv);
                if (aborted()) { stopped(mkvPart); return interrupted; }
                if (!okMp4 || !okMkv) {
                    DeleteFileW(mkvPart.c_str());
                    LOGE("job: MKV verification FAILED: ffprobe could not read the duration of the %s", !okMkv ? "MKV" : "MP4");
                    fail(L"could not verify the re-encoded MKV (ffprobe could not read its duration)");
                    return false;
                }
                const double tol = dMp4 * 0.02 > 1.0 ? dMp4 * 0.02 : 1.0;
                if (fabs(dMp4 - dMkv) > tol) {
                    DeleteFileW(mkvPart.c_str());
                    LOGE("job: MKV verification FAILED: duration %.3f s vs MP4 %.3f s (allowed difference %.2f s)", dMkv, dMp4, tol);
                    fail(L"the re-encoded MKV's duration differs from the MP4's");
                    return false;
                }
                LOGI("job: MKV verification (re-encode): exists, %llu bytes > 0, duration %.3f s vs MP4 %.3f s (within %.2f s) -- ok",
                     mkvSize, dMkv, dMp4, tol);
            }
        }
        if (!commit("MKV", mkvPart, mkv, mkv)) { fail(L"could not rename the verified MKV to its final name"); return false; }
        mkvOk = true;
        LOGI("job: MKV step ok in %.1f s (%s, %llu bytes, MP4 %llu bytes) -> %s", sec, reencode ? "re-encoded" : "remuxed", mkvSize, mp4Size,
             N(FileName(mkv)).c_str());
        StepDone("MKV", mkv);
    }

    if (runFlat) {
        SetStep(L"flat");
        // Flat SBS: left half, 16:9 centre crop. Dome: the left eye's view straight ahead as a normal 16:9 picture,
        // 100 x 67.7 degrees (v360; vr180 = left half, half-equirect; vr360 = top half, full equirect).
        std::wstring vf = L"crop=iw/2:ih:0:0,crop=iw:min(ih\\,iw*9/16),scale=3840:-2";
        if (rec.packed) {
            // recorded in the YouTube layout: take the left eye (top / left part) and undo the half-format squeeze first
            // (setsar=1: the scaler would otherwise keep the squeezed display aspect in the sample aspect ratio)
            const wchar_t* eye = rec.packing == Layout3d::TbHalf  ? L"crop=iw:ih/2:0:0,scale=iw:ih*2,setsar=1"
                               : rec.packing == Layout3d::TbFull  ? L"crop=iw:ih/2:0:0"
                               : rec.packing == Layout3d::SbsHalf ? L"crop=iw/2:ih:0:0,scale=iw*2:ih,setsar=1"
                                                                  : L"crop=iw/2:ih:0:0";
            vf = std::wstring(eye) + L",crop=iw:min(ih\\,iw*9/16),scale=3840:-2";
            LOGI("job: flat step: recorded in the YouTube layout (%s): left eye = %s", packedDesc.c_str(), N(eye).c_str());
        } else if (rec.projection == Recording::Vr180)
            vf = L"crop=iw/2:ih:0:0,v360=input=hequirect:output=flat:h_fov=100:v_fov=67.7:w=3840:h=2160";
        else if (rec.projection == Recording::Vr360)
            vf = L"crop=iw:ih/2:0:0,v360=input=equirect:output=flat:h_fov=100:v_fov=67.7:w=3840:h=2160";
        const std::wstring input = Q(mp4) + L" -map 0:v:0 -map 0:a? -vf \"" + vf + L"\" ";
        const std::wstring tail = L" -c:a copy -movflags +faststart -f mp4 " + Q(flatPart);
        const std::vector<EncTry> tries = {{"hevc_nvenc", L"-c:v hevc_nvenc -preset p5 -rc vbr -b:v 40M -maxrate 80M -tag:v hvc1", 1, nullptr},
                                           {"libx264", L"-c:v libx264 -preset medium -crf 18", 0, nullptr}};
        const ULONGLONG s0 = GetTickCount64();
        bool ran = false;
        if (!encode("flat", input, tries, nullptr, tail, flatPart, ran)) return interrupted;
        const double sec = (GetTickCount64() - s0) / 1000.0;
        unsigned long long flatSize = 0;
        if (!ran || code != 0 || !FileSize(flatPart, flatSize) || flatSize == 0) {
            DeleteFileW(flatPart.c_str());
            std::wstring why = FirstLine(out);
            if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
            LOGE("job: flat step FAILED after %.1f s: %s", sec, N(why).c_str());
            fail(L"flat version: " + why);   // the MKV (if made) stays, and so does the MP4
            return false;
        }
        if (!commit("flat", flatPart, flat, flat)) { fail(L"flat version: could not rename the finished file to its final name"); return false; }
        flatOk = true;
        LOGI("job: flat step ok in %.1f s (%llu bytes) -> %s", sec, flatSize, N(FileName(flat)).c_str());
        StepDone("flat", flat);
    }

    // 8K copy for YouTube (layout: layout_3d, default tb_half): YouTube puts a 4640 / 5804 wide SBS frame in its 4K tier (1920 per eye, low bitrate); a frame
    // with a short side of 4320 gets the 8K tier and about 3x the bitrate. YouTube reads a 3D upload as HALF format: it
    // assumes the aspect of the WHOLE frame is the aspect of ONE eye (a full 7680x4320 side-by-side looked 2x stretched
    // wide). So the default is half top-bottom: each eye keeps its own display aspect, is squeezed to half its height and
    // the left eye goes on top; the frame is then exactly one eye's aspect. The other layouts (sbs_half, sbs_full, tb_full)
    // are for players that read them. For all four the FRAME's short side is S (4320) and its long side is capped at 8192
    // (BuildLayoutGraph, shared with the MKV step, which uses the native size instead).
    if (cfg.postYt8k && (fixedLayout || runYt8k)) {   // (done before the restart: prevYt8k, nothing to run)
        SetStep(L"8K");
        if (rec.Dome()) {
            LOGI("job: 8K step skipped: %s recordings are already full-frame", N(rec.Name()).c_str());
        } else if (rec.packed) {   // counts as ok, like a dome recording: no _YT8K file, keep_mp4 = 0 may delete the MP4
            LOGI("job: recorded in the YouTube layout (%s): already in the YouTube 8K layout -- 8K step skipped", packedDesc.c_str());
        } else {
            int shortSide = 4320;
            wchar_t sz[32];
            if (GetEnvironmentVariableW(L"EYECAP_TRAY_TEST_YT8K_SHORT", sz, 32) > 0) {   // test-only: small output
                int n = 0;
                wchar_t* end = nullptr;
                n = (int)wcstol(sz, &end, 10);
                if (end && *end == 0 && n >= 16 && n <= 8192 && n % 2 == 0) {
                    shortSide = n;
                    LOGW("job: 8K step: EYECAP_TRAY_TEST_YT8K_SHORT=%s (test) -- short side %d instead of 4320", N(sz).c_str(), n);
                } else {
                    LOGW("job: 8K step: EYECAP_TRAY_TEST_YT8K_SHORT=%s ignored (want an even number 16..8192)", N(sz).c_str());
                }
            }
            // YouTube plays at most 60 fps: cap a faster source (72 / 90 Hz headsets) with the fps filter. Without ffprobe the
            // frame rate is left alone. The cap goes first in the graph: same result as capping at the end, and the scalers
            // then see only the kept frames.
            std::wstring fpsCap;
            if (probe.empty()) {
                LOGW("job: 8K step: ffprobe.exe not found next to ffmpeg -- frame rate not checked (no 60 fps cap)");
            } else {
                const std::wstring pcmd = Q(probe) + L" -v error -select_streams v:0 -show_entries stream=r_frame_rate,avg_frame_rate"
                                                     L" -of default=nw=1 " + Q(mp4);
                double fps = 0.0;
                if (RunFfmpeg(pcmd, out, code) && code == 0 && ParseRate(ProbeValue(out, "r_frame_rate"), fps)) {
                    const std::string r = ProbeValue(out, "r_frame_rate"), a = ProbeValue(out, "avg_frame_rate");
                    if (fps > 60.0) {
                        fpsCap = L"fps=60,";
                        LOGI("job: 8K step: source r_frame_rate %s (%.3f fps, avg %s) > 60 -- capped to 60 fps (YouTube plays at most 60)",
                             r.c_str(), fps, a.c_str());
                    } else {
                        LOGI("job: 8K step: source r_frame_rate %s (%.3f fps, avg %s) <= 60 -- frame rate kept", r.c_str(), fps, a.c_str());
                    }
                } else {
                    if (aborted()) { stopped(L""); return interrupted; }
                    LOGW("job: 8K step: ffprobe could not read the frame rate (%s) -- no 60 fps cap", N(FirstLine(out)).c_str());
                }
            }
            const LayoutGraph g = BuildLayoutGraph(cfg.layout3d, cfg.eyeCrop, shortSide, fpsCap);
            LOGI("job: 8K step: layout_3d %s: %s; frame short side %d, long side <= 8192", g.name, g.rule, shortSide);
            logFrame("8K step", g, shortSide);
            const std::wstring input = Q(mp4) + L" -map 0:v:0 -map 0:a? -vf \"" + g.vf + L"\" ";
            const std::wstring tail = std::wstring(L" -c:a copy -max_muxing_queue_size 4096 -metadata:s:v:0 stereo_mode=") + g.stereoMode +
                                      L" -f matroska " + Q(yt8kPart);
            // yt8k_bitrate_mbps (default 150): VBR target, peak 4/3 of it, buffer 2x (150 -> 150M / 200M / 300M), the same for
            // both NVENC codecs. x265 is crf 18.
            const unsigned br = cfg.yt8kBitrateMbps;
            const std::wstring vbr = L" -rc vbr -b:v " + std::to_wstring(br) + L"M -maxrate " + std::to_wstring((br * 4 + 1) / 3) + L"M -bufsize " +
                                     std::to_wstring(br * 2) + L"M";
            const std::wstring encNv = L"-c:v hevc_nvenc -preset p5 -rc vbr -tier high -b:v " + std::to_wstring(br) + L"M -maxrate " +
                                       std::to_wstring((br * 4 + 1) / 3) + L"M -bufsize " + std::to_wstring(br * 2) + L"M";
            // yt8k_codec = av1: av1_nvenc (RTX 40 / 50), 8-bit 4:2:0 (-highbitdepth 0: YouTube takes 8-bit AV1 in MKV), high
            // tier (main tier caps AV1 level 6.x at 60-160 Mbit/s; 8K60 at 150-200 Mbit/s needs high), same preset and VBR
            // maths as HEVC, plus -multipass qres (a quarter-resolution first pass for the rate control: measured +9% time on an
            // 8K job, 11.2 -> 12.2 s, where the decode / scale dominates). When av1_nvenc is not listed or fails (no AV1 NVENC),
            // HEVC as before: hevc_nvenc, then libx265.
            // No libaom / libsvtav1 fallback: software AV1 at 8K is far slower than x265.
            const bool av1 = cfg.yt8kCodec == VideoCodec::Av1;
            const std::wstring encAv1 = L"-c:v av1_nvenc -preset p5 -tune hq -multipass qres -tier 1 -highbitdepth 0" + vbr;
            if (av1)
                LOGI("job: 8K step: yt8k_codec av1, yt8k_bitrate_mbps %u -> av1_nvenc %s (fallback: hevc_nvenc %s, then libx265 crf 18)", br,
                     N(encAv1).c_str(), N(encNv).c_str());
            else
                LOGI("job: 8K step: yt8k_bitrate_mbps %u -> hevc_nvenc %s (libx265 fallback: crf 18, no bitrate)", br, N(encNv).c_str());
            std::vector<EncTry> tries;
            if (av1) tries.push_back({"av1_nvenc", encAv1, 2, " (yt8k_codec av1: falling back to HEVC)"});
            tries.push_back({"hevc_nvenc", encNv, 1, nullptr});
            tries.push_back({"libx265", L"-c:v libx265 -preset medium -crf 18", 0, nullptr});
            const ULONGLONG s0 = GetTickCount64();
            bool ran = false;
            if (!encode("8K", input, tries, "8K step: no working hevc_nvenc -- libx265 at 8K is slow (many times the clip length)", tail, yt8kPart,
                        ran))
                return interrupted;
            const double sec = (GetTickCount64() - s0) / 1000.0;
            unsigned long long ySize = 0;
            if (!ran || code != 0 || !FileSize(yt8kPart, ySize) || ySize == 0) {
                DeleteFileW(yt8kPart.c_str());
                std::wstring why = FirstLine(out);
                if (why.empty()) why = L"ffmpeg exit code " + std::to_wstring(code);
                LOGE("job: 8K step FAILED after %.1f s: %s", sec, N(why).c_str());
                fail(L"8K version: " + why);   // the MKV / flat file (if made) stay, and so does the MP4
                return false;
            }
            // HEVC made it although the job asked for AV1: the _AV1 tag would lie, so the final name is the untagged one
            const bool untag = av1Tag && strcmp(usedEnc, "av1_nvenc") != 0;
            std::wstring dest;
            if (!commit("8K", yt8kPart, untag ? yt8kPlain : yt8k, dest)) {
                fail(L"8K version: could not rename the finished file to its final name");
                return false;
            }
            if (untag) LOGI("job: 8K step: made by %s, not av1_nvenc -- renamed %s to %s", usedEnc, N(FileName(yt8k)).c_str(), N(FileName(dest)).c_str());
            yt8k = dest;
            yt8kOk = true;
            LOGI("job: 8K step ok in %.1f s (%llu bytes) -> %s", sec, ySize, N(FileName(yt8k)).c_str());
            StepDone("8K", yt8k);
        }
    }

    // Delete the MP4 only when the verified MKV replaces it and every requested step worked. Last line of defence: only a
    // source named *.mp4 is ever deleted (a picked .mkv / _FLAT.mp4 is refused by the tray, but never trust that here).
    std::wstring deleted;
    size_t needed = 0;   // other waiting jobs for this MP4 (any format): it stays until the last of them
    const bool srcIsMp4 = EndsWithI(mp4, L".mp4");
    const bool mayDelete = mkvOk && !cfg.keepMp4 && srcIsMp4;
    if (mkvOk && !cfg.keepMp4 && !srcIsMp4) LOGW("job: keep_mp4 = 0, but %s is not an .mp4 recording -- never deleted", N(name).c_str());
    std::unique_lock<std::mutex> delLock(m_, std::defer_lock);   // held while deleting: no job for it can be queued meanwhile
    if (mayDelete) {
        delLock.lock();
        for (const auto& q : queue_) if (_wcsicmp(q.path.c_str(), mp4.c_str()) == 0) ++needed;
    }
    if (mayDelete && needed) {
        LOGI("job: keep_mp4 = 0, but %zu more queued job(s) need %s -- not deleted yet (the last job for it decides)", needed, N(name).c_str());
    } else if (mayDelete) {
        if (DeleteFileW(mp4.c_str())) {
            LOGI("job: deleted %s (keep_mp4 = 0)", N(name).c_str());
            deleted = L" (MP4 removed)";
            if (rec.hasSidecar) {   // the MKV carries the projection itself; the sidecar only described the MP4
                if (DeleteFileW(rec.sidecar.c_str())) LOGI("job: deleted %s with the MP4", N(FileName(rec.sidecar)).c_str());
                else LOGW("job: could not delete %s (error %lu)", N(FileName(rec.sidecar)).c_str(), GetLastError());
            }
        } else {
            LOGW("job: could not delete %s (error %lu)", N(name).c_str(), GetLastError());
        }
    }
    if (delLock.owns_lock()) delLock.unlock();

    const std::wstring ready = rec.Dome() ? std::wstring(L"Ready for YouTube (") + rec.Name() + L"): " : std::wstring(L"Ready for YouTube: ");
    std::wstring mainOut, text;
    if (mkvOk) { mainOut = FileName(mkv); text = ready + mainOut; }
    else if (yt8kOk) { mainOut = FileName(yt8k); text = L"Ready for YouTube (8K): " + mainOut; }
    else if (flatOk) { mainOut = FileName(flat); text = L"Flat 2D version ready: " + mainOut; }
    if (mkvOk && yt8kOk) text += L"\nYouTube 8K: " + FileName(yt8k);
    if (flatOk && (mkvOk || yt8kOk)) text += L"\nFlat 2D: " + FileName(flat);
    if (mainOut.empty()) {   // only the 8K step was requested and it was skipped (dome / packed recording): nothing was made
        LOGI("job: done %s in %.1f s -- nothing made (8K step skipped)", N(name).c_str(), (GetTickCount64() - t0) / 1000.0);
        Notice skip{L"Nothing to convert", name + L"\n" +
                        (rec.packed ? std::wstring(L"This recording is already in the YouTube layout; choose the 3D MKV output (a quick copy) to upload it.")
                                    : std::wstring(rec.Name()) + L" recordings are already full-frame; the 8K copy is for flat 3D recordings."),
                    false};
        Finish(true, L"last: " + name + L" (8K copy skipped: " + rec.Name() + L")", &skip);
        return false;
    }
    Notice n{L"VR EyeCapture", text, false};
    LOGI("job: done %s in %.1f s%s", N(mainOut).c_str(), (GetTickCount64() - t0) / 1000.0, N(deleted).c_str());
    Finish(true, L"last: " + mainOut + L" ready", &n);
    return false;
}

} // namespace pp
