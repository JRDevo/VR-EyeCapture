// openvr_versions -- the IVRCompositor / IVRSystem versions the SteamVR proxy knows, with the method slots it hooks or
// calls. From the history of Valve's openvr.h:
//   * IVRCompositor: SetTrackingSpace, GetTrackingSpace, WaitGetPoses (2), GetLastPoses, GetLastPoseForTrackedDeviceIndex,
//     Submit (5) -- the same first six methods since IVRCompositor_014. Later versions only appended methods (and _028
//     inserted SubmitWithArrayIndex at 6, after Submit), so the two hooked slots never moved.
//   * methodCount: the size of the "FnTable:" function table of that version, for the copy the proxy hands out -- an
//     ESTIMATE from the openvr.h history (a review recalled different counts for some versions, e.g. _022 = 43; not
//     verified either way). It does not need to be exact: the copy
//     takes kFnTableCopyMargin more entries than this (guarded against an unreadable page), so a count that is a little
//     low never leaves a method out; the game never calls past its own version's count.
//   * IVRSystem: only called (never hooked): GetProjectionRaw (2), GetEyeToHeadTransform (4) and the float property
//     getter (22 since IVRSystem_021, which dropped ResetSeatedZeroPose; 23 before). The neighbour slots are the bool /
//     int32 property getters with the same signature, so the caller checks the error code and the value.
// A version not in these tables: logged once, forwarded untouched (that game runs, unrecorded).
#pragma once
#include <cstdint>
#include <cstring>

struct CompositorVersion {
    const char* name;           // "IVRCompositor_028"
    uint32_t    methodCount;    // entries of the FnTable of this version (estimate, see kFnTableCopyMargin)
    uint32_t    slotWaitGetPoses;
    uint32_t    slotSubmit;
};

const CompositorVersion kCompositorVersions[] = {
    {"IVRCompositor_019", 40, 2, 5},
    {"IVRCompositor_020", 41, 2, 5},
    {"IVRCompositor_021", 41, 2, 5},
    {"IVRCompositor_022", 41, 2, 5},
    {"IVRCompositor_023", 43, 2, 5},
    {"IVRCompositor_024", 43, 2, 5},
    {"IVRCompositor_025", 46, 2, 5},
    {"IVRCompositor_026", 48, 2, 5},
    {"IVRCompositor_027", 51, 2, 5},
    {"IVRCompositor_028", 52, 2, 5},
};
const int kCompositorVersionCount = (int)(sizeof(kCompositorVersions) / sizeof(kCompositorVersions[0]));
const uint32_t kFnTableCopyMargin = 16;
const uint32_t kFnTableMax = 96;   // our static copies are this long

struct SystemVersion {
    const char* name;              // "IVRSystem_022"
    uint32_t    slotProjectionRaw;
    uint32_t    slotEyeToHead;
    uint32_t    slotFloatProperty;
};

// Asked in this order by the proxy (newest first).
const SystemVersion kSystemVersions[] = {
    {"IVRSystem_022", 2, 4, 22},
    {"IVRSystem_021", 2, 4, 22},
    {"IVRSystem_020", 2, 4, 23},
    {"IVRSystem_019", 2, 4, 23},
};
const int kSystemVersionCount = (int)(sizeof(kSystemVersions) / sizeof(kSystemVersions[0]));

// IVROverlay (only called, never hooked; the in-headset recording dot, src/openvr/openvr_indicator.cpp): the proxy asks the
// real DLL itself for "FnTable:IVROverlay_0xx" (newest first) -- SteamVR serves older versions too, whatever the game uses.
// Slots = positions in the VR_IVROverlay_FnTable struct of Valve's openvr_capi.h of that version, checked 2026-10-18 against
// the headers of openvr v2.5.1 (_027), v1.23.7 (_026) and v1.16.8 (_025). The signatures of these methods are the same in
// all three (C function tables: no this pointer, the overlay handle is a uint64_t).
struct OverlayVersion {
    const char* name;                 // "IVROverlay_027" (asked as "FnTable:" + name)
    uint32_t create, destroy, alpha, sortOrder, width, transformDeviceRelative, show, hide, setTexture;
};
const OverlayVersion kOverlayVersions[] = {
    {"IVROverlay_027", 1, 2, 15, 19, 21, 34, 41, 42, 58},
    {"IVROverlay_026", 1, 2, 15, 19, 21, 34, 43, 44, 60},
    {"IVROverlay_025", 1, 2, 15, 19, 21, 32, 41, 42, 57},
};
const int kOverlayVersionCount = (int)(sizeof(kOverlayVersions) / sizeof(kOverlayVersions[0]));

const char kFnTablePrefix[] = "FnTable:";

// "IVRCompositor_028" / "FnTable:IVRCompositor_028" -> index into kCompositorVersions, *fnTable = the FnTable form.
// -1 = not an IVRCompositor name (*isCompositor = false) or a version we do not know (*isCompositor = true).
inline int FindCompositorVersion(const char* name, bool* fnTable, bool* isCompositor) {
    if (fnTable) *fnTable = false;
    if (isCompositor) *isCompositor = false;
    if (!name) return -1;
    const size_t pl = sizeof(kFnTablePrefix) - 1;
    if (strncmp(name, kFnTablePrefix, pl) == 0) {
        name += pl;
        if (fnTable) *fnTable = true;
    }
    if (strncmp(name, "IVRCompositor_", 14) != 0) return -1;
    if (isCompositor) *isCompositor = true;
    for (int i = 0; i < kCompositorVersionCount; ++i)
        if (strcmp(name, kCompositorVersions[i].name) == 0) return i;
    return -1;
}
