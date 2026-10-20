// openvr_capture -- the SteamVR proxy's capture front end (the OpenXR layer's counterpart for OpenVR games).
//
// proxy_exports.cpp owns the exports and the real openvr_api_real.dll; it calls in here:
//   OvrProcessInit       once, at the first export call that needs it: home discovery (sidecar / registry), log,
//                        config, banner. No home = pure forwarder for the whole process (no log, nothing written).
//   OvrOnInit            after the real VR_InitInternal(2) succeeded: arm check (tray / require_tray), control thread.
//   OvrOnInterface       after the real VR_GetGenericInterface: an IVRCompositor of a known version gets its WaitGetPoses /
//                        Submit hooked -- the C++ object's vtable patched in place, or a copy of the FnTable handed out.
//   OvrOnShutdown        before the real VR_ShutdownInternal: finalize a recording, publish, unpatch, stop the control
//                        thread (the real shutdown unloads vrclient, which owns the patched vtables).
// Per frame: the hooked Submit pairs Left + Right (either order) and, on the second eye, copies both eye images BEFORE
// forwarding that Submit (Recorder::SubmitFrame + LiveShare::Frame), exactly as the layer does inside xrEndFrame: D3D11
// textures on the game's immediate context, D3D12 textures (D3D12TextureData_t) by a command list of ours on the game's
// queue from the Submit (PIXEL_SHADER_RESOURCE -> COPY_SOURCE -> back), fence-synchronized with the encoder, no CPU wait. Every call is forwarded and its result returned unchanged; a fault in the capture disables it.
#pragma once
#include <cstdint>
#include <string>
#include "../../third_party/openvr/openvr.h"

// realLoaded / realPath / loadError: the outcome of LoadLibraryW(<proxy dir>\openvr_api_real.dll), for the banner.
void OvrProcessInit(const std::wstring& proxyDir, bool realLoaded, const std::wstring& realPath, unsigned long loadError);
// A home was found and steamvr_enabled = 1 (else every export is a plain forward).
bool OvrActive();
void OvrOnInit(const char* fn, vr::EVRApplicationType appType, vr::EVRInitError err, uint32_t token);
// Returns what the game gets: `iface` itself (vtable patched or untouched) or our FnTable copy.
void* OvrOnInterface(const char* name, void* iface, PFN_VR_GetGenericInterface realGet);
void OvrOnShutdown();
// The real DLL is missing / lacks an export (logged once per export name).
void OvrLogMissingExport(const char* name);
