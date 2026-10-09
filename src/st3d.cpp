// st3d -- see st3d.h.
#include "st3d.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool Seek(uint64_t off) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
    }
    bool Read(void* p, size_t n) {
        uint8_t* b = (uint8_t*)p;
        while (n) {
            DWORD chunk = (DWORD)(n > (1u << 30) ? (1u << 30) : n), got = 0;
            if (!ReadFile(h, b, chunk, &got, nullptr) || got != chunk) return false;
            b += got; n -= got;
        }
        return true;
    }
    bool Write(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        while (n) {
            DWORD chunk = (DWORD)(n > (1u << 30) ? (1u << 30) : n), put = 0;
            if (!WriteFile(h, b, chunk, &put, nullptr) || put != chunk) return false;
            b += put; n -= put;
        }
        return true;
    }
    uint64_t Size() { LARGE_INTEGER li{}; GetFileSizeEx(h, &li); return (uint64_t)li.QuadPart; }
};

uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t BE64(const uint8_t* p) { return (uint64_t)BE32(p) << 32 | BE32(p + 4); }
void PutBE32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void PutBE64(uint8_t* p, uint64_t v) { PutBE32(p, (uint32_t)(v >> 32)); PutBE32(p + 4, (uint32_t)v); }

struct Box { uint64_t off, size; uint32_t hdr; char type[5]; };

// Parse the box at buf[off] (in-memory, inside [off, end)).
bool ParseBox(const std::vector<uint8_t>& buf, uint64_t off, uint64_t end, Box& b) {
    if (off + 8 > end) return false;
    uint64_t size = BE32(&buf[(size_t)off]);
    uint32_t hdr = 8;
    if (size == 1) { if (off + 16 > end) return false; size = BE64(&buf[(size_t)off + 8]); hdr = 16; }
    else if (size == 0) size = end - off;
    if (size < hdr || off + size > end) return false;
    b.off = off; b.size = size; b.hdr = hdr;
    memcpy(b.type, &buf[(size_t)off + 4], 4); b.type[4] = 0;
    return true;
}

bool FindChild(const std::vector<uint8_t>& buf, const Box& parent, uint64_t childStart, const char* type, Box& out) {
    uint64_t p = childStart;
    const uint64_t end = parent.off + parent.size;
    Box b;
    while (p < end && ParseBox(buf, p, end, b)) {
        if (memcmp(b.type, type, 4) == 0) { out = b; return true; }
        p += b.size;
    }
    return false;
}

void GrowBox(std::vector<uint8_t>& buf, const Box& b, uint32_t delta) {
    if (b.hdr == 16) PutBE64(&buf[(size_t)b.off + 8], b.size + delta);
    else PutBE32(&buf[(size_t)b.off], (uint32_t)(b.size + delta));
}

// Shift every stco/co64 entry in moov by delta (all tracks).
bool ShiftChunkOffsets(std::vector<uint8_t>& buf, const Box& box, uint64_t childStart, uint32_t delta) {
    uint64_t p = childStart;
    const uint64_t end = box.off + box.size;
    Box b;
    while (p < end && ParseBox(buf, p, end, b)) {
        const uint64_t body = b.off + b.hdr;
        if (!memcmp(b.type, "trak", 4) || !memcmp(b.type, "mdia", 4) || !memcmp(b.type, "minf", 4) || !memcmp(b.type, "stbl", 4)) {
            if (!ShiftChunkOffsets(buf, b, body, delta)) return false;
        } else if (!memcmp(b.type, "stco", 4)) {
            const uint32_t n = BE32(&buf[(size_t)body + 4]);
            if (body + 8 + 4ull * n > b.off + b.size) return false;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t* e = &buf[(size_t)(body + 8 + 4ull * i)];
                const uint64_t v = (uint64_t)BE32(e) + delta;
                if (v > 0xFFFFFFFFull) return false;
                PutBE32(e, (uint32_t)v);
            }
        } else if (!memcmp(b.type, "co64", 4)) {
            const uint32_t n = BE32(&buf[(size_t)body + 4]);
            if (body + 8 + 8ull * n > b.off + b.size) return false;
            for (uint32_t i = 0; i < n; ++i) {
                uint8_t* e = &buf[(size_t)(body + 8 + 8ull * i)];
                PutBE64(e, BE64(e) + delta);
            }
        }
        p += b.size;
    }
    return true;
}

} // namespace

bool PatchSt3d(const std::wstring& path, unsigned char stereoMode, std::string& report) {
    char msg[256];
    File f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) { report = "st3d: cannot open the file for writing"; return false; }
    const uint64_t fileSize = f.Size();

    // --- top-level scan ---
    uint64_t moovOff = 0, moovSize = 0, mdatOff = 0;
    bool haveMoov = false, haveMdat = false;
    uint64_t pos = 0;
    while (pos + 8 <= fileSize) {
        uint8_t h[16];
        if (!f.Seek(pos) || !f.Read(h, 8)) break;
        uint64_t size = BE32(h);
        if (size == 1) { if (!f.Read(h + 8, 8)) break; size = BE64(h + 8); }
        else if (size == 0) size = fileSize - pos;
        if (size < 8 || pos + size > fileSize) break;
        if (!memcmp(h + 4, "moov", 4)) { moovOff = pos; moovSize = size; haveMoov = true; }
        if (!memcmp(h + 4, "mdat", 4) && !haveMdat) { mdatOff = pos; haveMdat = true; }
        pos += size;
    }
    if (!haveMoov) { report = "st3d: no moov box (file not finalized?)"; return false; }
    if (moovSize > 256ull * 1024 * 1024) { report = "st3d: moov too large"; return false; }

    // --- load moov ---
    std::vector<uint8_t> moov((size_t)moovSize);
    if (!f.Seek(moovOff) || !f.Read(moov.data(), moov.size())) { report = "st3d: read moov failed"; return false; }
    Box mv;
    if (!ParseBox(moov, 0, moov.size(), mv)) { report = "st3d: bad moov"; return false; }

    // --- find the video track's sample entry ---
    Box trak, mdia, hdlr, minf, stbl, stsd, entry;
    bool found = false;
    uint64_t p = mv.hdr;
    Box b;
    while (!found && p < mv.size && ParseBox(moov, p, mv.size, b)) {
        p += b.size;
        if (memcmp(b.type, "trak", 4)) continue;
        trak = b;
        if (!FindChild(moov, trak, trak.off + trak.hdr, "mdia", mdia)) continue;
        if (!FindChild(moov, mdia, mdia.off + mdia.hdr, "hdlr", hdlr)) continue;
        // hdlr: version/flags(4) pre_defined(4) handler_type(4)
        if (hdlr.size < hdlr.hdr + 12 || memcmp(&moov[(size_t)(hdlr.off + hdlr.hdr + 8)], "vide", 4)) continue;
        if (!FindChild(moov, mdia, mdia.off + mdia.hdr, "minf", minf)) continue;
        if (!FindChild(moov, minf, minf.off + minf.hdr, "stbl", stbl)) continue;
        if (!FindChild(moov, stbl, stbl.off + stbl.hdr, "stsd", stsd)) continue;
        // stsd: version/flags(4) entry_count(4) then entries
        const uint64_t e0 = stsd.off + stsd.hdr + 8;
        if (!ParseBox(moov, e0, stsd.off + stsd.size, entry)) continue;
        if (memcmp(entry.type, "hvc1", 4) && memcmp(entry.type, "hev1", 4) && memcmp(entry.type, "avc1", 4) &&
            memcmp(entry.type, "avc3", 4)) {
            snprintf(msg, sizeof(msg), "st3d: unexpected video sample entry '%s'", entry.type);
            report = msg;
            return false;
        }
        found = true;
    }
    if (!found) { report = "st3d: no video track sample entry found"; return false; }

    // VisualSampleEntry fixed part is 78 bytes after the box header; children follow.
    const uint64_t childStart = entry.off + entry.hdr + 78;
    Box existing;
    if (childStart <= entry.off + entry.size && FindChild(moov, entry, childStart, "st3d", existing)) {
        snprintf(msg, sizeof(msg), "st3d: already present in '%s'", entry.type);
        report = msg;
        return true;
    }

    // --- build the new moov ---
    const uint32_t kDelta = 13;
    const uint8_t st3d[13] = {0, 0, 0, 13, 's', 't', '3', 'd', 0, 0, 0, 0, stereoMode};
    for (const Box* a : {&mv, &trak, &mdia, &minf, &stbl, &stsd, &entry}) GrowBox(moov, *a, kDelta);
    moov.insert(moov.begin() + (ptrdiff_t)(entry.off + entry.size), st3d, st3d + kDelta);
    // mv was parsed before the insert; size field already updated.

    const bool moovBeforeMdat = haveMdat && moovOff < mdatOff;
    const uint64_t afterMoov = moovOff + moovSize;

    if (!moovBeforeMdat) {
        // In place: rewrite moov + whatever follows it (no mdat follows, so it is small).
        const uint64_t tailSize = fileSize - afterMoov;
        if (tailSize > 64ull * 1024 * 1024) { report = "st3d: unexpected large tail after moov"; return false; }
        std::vector<uint8_t> tail((size_t)tailSize);
        if (tailSize && (!f.Seek(afterMoov) || !f.Read(tail.data(), tail.size()))) { report = "st3d: read tail failed"; return false; }
        if (!f.Seek(moovOff) || !f.Write(moov.data(), moov.size()) || (tailSize && !f.Write(tail.data(), tail.size()))) {
            report = "st3d: write failed (file may be damaged)";
            return false;
        }
        FlushFileBuffers(f.h);
        snprintf(msg, sizeof(msg), "st3d: added stereo_mode=%u to '%s' (moov after mdat, patched in place)", stereoMode, entry.type);
        report = msg;
        return true;
    }

    // moov before mdat: try to absorb the growth in a following 'free' box.
    {
        uint8_t h[8];
        if (afterMoov + 8 <= fileSize && f.Seek(afterMoov) && f.Read(h, 8) && !memcmp(h + 4, "free", 4)) {
            const uint32_t freeSize = BE32(h);
            if (freeSize >= 8 + kDelta) {
                uint8_t nh[8];
                PutBE32(nh, freeSize - kDelta);
                memcpy(nh + 4, "free", 4);
                if (!f.Seek(moovOff) || !f.Write(moov.data(), moov.size()) || !f.Write(nh, 8)) {
                    report = "st3d: write failed (file may be damaged)";
                    return false;
                }
                FlushFileBuffers(f.h);
                snprintf(msg, sizeof(msg), "st3d: added stereo_mode=%u to '%s' (moov before mdat, used free box)", stereoMode, entry.type);
                report = msg;
                return true;
            }
        }
    }

    // Full rewrite with shifted chunk offsets.
    {
        Box mv2;
        ParseBox(moov, 0, moov.size(), mv2);
        if (!ShiftChunkOffsets(moov, mv2, mv2.hdr, kDelta)) { report = "st3d: chunk offset shift failed"; return false; }
    }
    const std::wstring tmp = path + L".st3d.tmp";
    File o;
    o.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o.h == INVALID_HANDLE_VALUE) { report = "st3d: cannot create temp file"; return false; }
    std::vector<uint8_t> chunk(8u << 20);
    auto copyRange = [&](uint64_t from, uint64_t len) {
        if (!f.Seek(from)) return false;
        while (len) {
            const size_t n = (size_t)(len < chunk.size() ? len : chunk.size());
            if (!f.Read(chunk.data(), n) || !o.Write(chunk.data(), n)) return false;
            len -= n;
        }
        return true;
    };
    bool ok = copyRange(0, moovOff) && o.Write(moov.data(), moov.size()) && copyRange(afterMoov, fileSize - afterMoov);
    CloseHandle(o.h); o.h = INVALID_HANDLE_VALUE;
    CloseHandle(f.h); f.h = INVALID_HANDLE_VALUE;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        report = "st3d: rewrite failed (original file left unchanged)";
        return false;
    }
    snprintf(msg, sizeof(msg), "st3d: added stereo_mode=%u to '%s' (moov before mdat, file rewritten, offsets +13)", stereoMode, entry.type);
    report = msg;
    return true;
}
