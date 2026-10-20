// openvr.h -- MINIMAL subset of Valve's OpenVR SDK header (headers/openvr.h, https://github.com/ValveSoftware/openvr),
// hand-transcribed for VR EyeCapture's SteamVR proxy: only the types, enums and interface method orders the proxy and its
// tests use. No code, no library. Layouts follow the OpenVR SDK; see the notes at IVRSystem / IVRCompositor for which
// method slots are relied on.
//
// Copyright (c) 2015, Valve Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
//    disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
//    products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once
#include <cstdint>

struct ID3D12Resource;
struct ID3D12CommandQueue;

namespace vr {

typedef uint32_t TrackedDeviceIndex_t;
static const TrackedDeviceIndex_t k_unTrackedDeviceIndex_Hmd = 0;
static const uint32_t k_unMaxTrackedDeviceCount = 64;

struct HmdMatrix34_t { float m[3][4]; };
struct HmdMatrix33_t { float m[3][3]; };
struct HmdMatrix44_t { float m[4][4]; };
struct HmdVector3_t { float v[3]; };
struct HmdVector2_t { float v[2]; };
struct HmdQuaternion_t { double w, x, y, z; };

enum EVREye { Eye_Left = 0, Eye_Right = 1 };

enum ETextureType {
    TextureType_Invalid = -1,
    TextureType_DirectX = 0,            // ID3D11Texture2D
    TextureType_OpenGL = 1,
    TextureType_Vulkan = 2,
    TextureType_IOSurface = 3,
    TextureType_DirectX12 = 4,
    TextureType_DXGISharedHandle = 5,
    TextureType_Metal = 6,
    TextureType_Reserved = 7,
};

enum EColorSpace {
    ColorSpace_Auto = 0,    // the compositor decides from the texture format
    ColorSpace_Gamma = 1,   // gamma-encoded (sRGB) data
    ColorSpace_Linear = 2,  // linear data
};

struct Texture_t {
    void*        handle;      // DirectX: ID3D11Texture2D*; DirectX12: D3D12TextureData_t*
    ETextureType eType;
    EColorSpace  eColorSpace;
};

// TextureType_DirectX12: Texture_t::handle points to this. The compositor reads the resource on m_pCommandQueue (the queue
// the game rendered it on). Valve's D3D12 sample (hellovr_dx12) transitions the eye texture to
// D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE before Submit and back to RENDER_TARGET for the next frame.
struct D3D12TextureData_t {
    ID3D12Resource*     m_pResource;
    ID3D12CommandQueue* m_pCommandQueue;
    uint32_t            m_nNodeMask;
};

enum ETrackingResult {
    TrackingResult_Uninitialized = 1,
    TrackingResult_Calibrating_InProgress = 100,
    TrackingResult_Calibrating_OutOfRange = 101,
    TrackingResult_Running_OK = 200,
    TrackingResult_Running_OutOfRange = 201,
    TrackingResult_Fallback_RotationOnly = 300,
};

enum ETrackingUniverseOrigin {
    TrackingUniverseSeated = 0,
    TrackingUniverseStanding = 1,
    TrackingUniverseRawAndUncalibrated = 2,
};

struct TrackedDevicePose_t {
    HmdMatrix34_t   mDeviceToAbsoluteTracking;
    HmdVector3_t    vVelocity;
    HmdVector3_t    vAngularVelocity;
    ETrackingResult eTrackingResult;
    bool            bPoseIsValid;
    bool            bDeviceIsConnected;
};

// u / v of the part of the texture to show; uMin > uMax / vMin > vMax flip it.
struct VRTextureBounds_t { float uMin, vMin; float uMax, vMax; };

// Submit_TextureWithPose
struct VRTextureWithPose_t : public Texture_t {
    HmdMatrix34_t mDeviceToAbsoluteTracking;   // the pose the frame was rendered with
};

struct VRTextureDepthInfo_t {
    void*         handle;
    HmdMatrix44_t mProjection;
    HmdVector2_t  vRange;
};
struct VRTextureWithDepth_t : public Texture_t { VRTextureDepthInfo_t depth; };
struct VRTextureWithPoseAndDepth_t : public VRTextureWithPose_t { VRTextureDepthInfo_t depth; };

enum EVRSubmitFlags {
    Submit_Default = 0x00,
    Submit_LensDistortionAlreadyApplied = 0x01,
    Submit_GlRenderBuffer = 0x02,
    Submit_Reserved = 0x04,
    Submit_TextureWithPose = 0x08,
    Submit_TextureWithDepth = 0x10,
    Submit_FrameDiscontinuty = 0x20,
    Submit_VulkanTextureWithArrayData = 0x40,
    Submit_GlArrayTexture = 0x80,
    Submit_IsEgl = 0x100,
    Submit_Reserved2 = 0x08000,
    Submit_Reserved3 = 0x10000,
};

enum EVRCompositorError {
    VRCompositorError_None = 0,
    VRCompositorError_RequestFailed = 1,
    VRCompositorError_IncompatibleVersion = 100,
    VRCompositorError_DoNotHaveFocus = 101,
    VRCompositorError_InvalidTexture = 102,
    VRCompositorError_IsNotSceneApplication = 103,
    VRCompositorError_TextureIsOnWrongDevice = 104,
    VRCompositorError_TextureUsesUnsupportedFormat = 105,
    VRCompositorError_SharedTexturesNotSupported = 106,
    VRCompositorError_IndexOutOfRange = 107,
    VRCompositorError_AlreadySubmitted = 108,
    VRCompositorError_InvalidBounds = 109,
    VRCompositorError_AlreadySet = 110,
};

enum EVRApplicationType {
    VRApplication_Other = 0,
    VRApplication_Scene = 1,
    VRApplication_Overlay = 2,
    VRApplication_Background = 3,
    VRApplication_Utility = 4,
    VRApplication_VRMonitor = 5,
    VRApplication_SteamWatchdog = 6,
    VRApplication_Bootstrapper = 7,
    VRApplication_WebHelper = 8,
    VRApplication_OpenXRInstance = 9,
    VRApplication_OpenXRScene = 10,
    VRApplication_OpenXROverlay = 11,
    VRApplication_Prism = 12,
    VRApplication_RoomView = 13,
    VRApplication_Max
};

// Only the values the proxy and the mock use (the real enum has ~150).
enum EVRInitError {
    VRInitError_None = 0,
    VRInitError_Unknown = 1,
    VRInitError_Init_InstallationNotFound = 100,
    VRInitError_Init_InstallationCorrupt = 101,
    VRInitError_Init_VRClientDLLNotFound = 102,
    VRInitError_Init_FileNotFound = 103,
    VRInitError_Init_FactoryNotFound = 104,
    VRInitError_Init_InterfaceNotFound = 105,
    VRInitError_Init_InvalidInterface = 106,
    VRInitError_Init_UserConfigDirectoryInvalid = 107,
    VRInitError_Init_HmdNotFound = 108,
    VRInitError_Init_NotInitialized = 109,
    VRInitError_Init_NoServerForBackgroundApp = 121,   // VRApplication_Background and no vrserver running
};

enum ETrackedDeviceProperty {
    Prop_Invalid = 0,
    Prop_DisplayFrequency_Float = 2002,
};

enum ETrackedPropertyError {
    TrackedProp_Success = 0,
    TrackedProp_WrongDataType = 1,
    TrackedProp_WrongDeviceClass = 2,
    TrackedProp_BufferTooSmall = 3,
    TrackedProp_UnknownProperty = 4,
    TrackedProp_InvalidDevice = 5,
    TrackedProp_CouldNotContactServer = 6,
    TrackedProp_ValueNotProvidedByDevice = 7,
    TrackedProp_StringExceedsMaximumLength = 8,
    TrackedProp_NotYetAvailable = 9,
    TrackedProp_PermissionDenied = 10,
    TrackedProp_InvalidOperation = 11,
    TrackedProp_CannotWriteToWildcards = 12,
    TrackedProp_IPCReadFailure = 13,
    TrackedProp_OutOfMemory = 14,
    TrackedProp_InvalidContainer = 15,
};

struct DistortionCoordinates_t { float rfRed[2]; float rfGreen[2]; float rfBlue[2]; };

// VREvent_t: eventType, trackedDeviceIndex, eventAgeSeconds, then the VREvent_Data_t union (its largest member,
// VREvent_Reserved_t, is six uint64: 48 bytes, 8-byte aligned) -> 64 bytes on Windows x64. PollNextEvent is passed this
// size and only fills a struct of a size it knows. Only the event types the mirror recorder reacts to are named.
enum EVREventType {
    VREvent_None = 0,
    VREvent_Quit = 700,                 // SteamVR is closing: apps should exit
    VREvent_ProcessQuit = 701,
    VREvent_QuitAcknowledged = 703,
};
struct VREvent_t {
    uint32_t eventType;
    TrackedDeviceIndex_t trackedDeviceIndex;
    float eventAgeSeconds;
    uint64_t data[6];
};
static_assert(sizeof(VREvent_t) == 64, "VREvent_t is 64 bytes in the OpenVR ABI (x64)");
struct VkInstance_T;

// IVRSystem, method order of IVRSystem_021 / _022 up to GetFloatTrackedDeviceProperty (the proxy relies on slots 2
// GetProjectionRaw, 4 GetEyeToHeadTransform -- stable since the first versions -- and, guarded, on the float property
// slot: 22 here; IVRSystem_019 / _020 still had ResetSeatedZeroPose after GetDeviceToAbsoluteTrackingPose, so 23 there.
// The neighbouring slots (GetBool / GetInt32 ...TrackedDeviceProperty) share the signature; the proxy checks the error
// code and the value range). Not hooked: the proxy only calls it.
class IVRSystem {
public:
    virtual void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) = 0;                    // 0
    virtual HmdMatrix44_t GetProjectionMatrix(EVREye eEye, float fNearZ, float fFarZ) = 0;                      // 1
    virtual void GetProjectionRaw(EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) = 0; // 2
    virtual bool ComputeDistortion(EVREye eEye, float fU, float fV, DistortionCoordinates_t* pOut) = 0;          // 3
    virtual HmdMatrix34_t GetEyeToHeadTransform(EVREye eEye) = 0;                                               // 4
    virtual bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) = 0;          // 5
    virtual int32_t GetD3D9AdapterIndex() = 0;                                                                  // 6
    virtual void GetDXGIOutputInfo(int32_t* pnAdapterIndex) = 0;                                                // 7
    virtual void GetOutputDevice(uint64_t* pnDevice, ETextureType textureType, VkInstance_T* pInstance) = 0;    // 8
    virtual bool IsDisplayOnDesktop() = 0;                                                                      // 9
    virtual bool SetDisplayVisibility(bool bIsVisibleOnDesktop) = 0;                                            // 10
    virtual void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin eOrigin, float fPredictedSecondsToPhotonsFromNow,
                                                 TrackedDevicePose_t* pTrackedDevicePoseArray,
                                                 uint32_t unTrackedDevicePoseArrayCount) = 0;                    // 11
    virtual HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() = 0;                                // 12
    virtual HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() = 0;                                   // 13
    virtual uint32_t GetSortedTrackedDeviceIndicesOfClass(int eTrackedDeviceClass, TrackedDeviceIndex_t* punIndexArray,
                                                          uint32_t unIndexArrayCount,
                                                          TrackedDeviceIndex_t unRelativeToTrackedDeviceIndex) = 0; // 14
    virtual int GetTrackedDeviceActivityLevel(TrackedDeviceIndex_t unDeviceId) = 0;                             // 15
    virtual void ApplyTransform(TrackedDevicePose_t* pOutputPose, const TrackedDevicePose_t* pTrackedDevicePose,
                                const HmdMatrix34_t* pTransform) = 0;                                           // 16
    virtual TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(int unDeviceType) = 0;                  // 17
    virtual int GetControllerRoleForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex) = 0;                 // 18
    virtual int GetTrackedDeviceClass(TrackedDeviceIndex_t unDeviceIndex) = 0;                                  // 19
    virtual bool IsTrackedDeviceConnected(TrackedDeviceIndex_t unDeviceIndex) = 0;                              // 20
    virtual bool GetBoolTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                              ETrackedPropertyError* pError) = 0;                               // 21
    virtual float GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                                ETrackedPropertyError* pError) = 0;                             // 22
    // 23..29: the rest of the property getters and PollNextEvent (IVRSystem_021 / _022; the mirror recorder polls events)
    virtual int32_t GetInt32TrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                                  ETrackedPropertyError* pError) = 0;                           // 23
    virtual uint64_t GetUint64TrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                                    ETrackedPropertyError* pError) = 0;                         // 24
    virtual HmdMatrix34_t GetMatrix34TrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                                           ETrackedPropertyError* pError) = 0;                  // 25
    virtual uint32_t GetArrayTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                                   uint32_t propType, void* pBuffer, uint32_t unBufferSize,
                                                   ETrackedPropertyError* pError) = 0;                          // 26
    virtual uint32_t GetStringTrackedDeviceProperty(TrackedDeviceIndex_t unDeviceIndex, ETrackedDeviceProperty prop,
                                                    char* pchValue, uint32_t unBufferSize, ETrackedPropertyError* pError) = 0; // 27
    virtual const char* GetPropErrorNameFromEnum(ETrackedPropertyError error) = 0;                              // 28
    virtual bool PollNextEvent(VREvent_t* pEvent, uint32_t uncbVREvent) = 0;                                    // 29
};

// IVRCompositor, the first methods (stable order since IVRCompositor_014: WaitGetPoses = slot 2, Submit = slot 5). The
// method COUNT differs per version (src/openvr/openvr_versions.h). Hooked by the proxy: slots 2 and 5.
class IVRCompositor {
public:
    virtual void SetTrackingSpace(ETrackingUniverseOrigin eOrigin) = 0;                                          // 0
    virtual ETrackingUniverseOrigin GetTrackingSpace() = 0;                                                      // 1
    virtual EVRCompositorError WaitGetPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount,
                                            TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) = 0; // 2
    virtual EVRCompositorError GetLastPoses(TrackedDevicePose_t* pRenderPoseArray, uint32_t unRenderPoseArrayCount,
                                            TrackedDevicePose_t* pGamePoseArray, uint32_t unGamePoseArrayCount) = 0; // 3
    virtual EVRCompositorError GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t unDeviceIndex,
                                                                TrackedDevicePose_t* pOutputPose,
                                                                TrackedDevicePose_t* pOutputGamePose) = 0;      // 4
    virtual EVRCompositorError Submit(EVREye eEye, const Texture_t* pTexture, const VRTextureBounds_t* pBounds,
                                      EVRSubmitFlags nSubmitFlags) = 0;                                         // 5
};

// The C (FnTable) form of the first IVRCompositor entries ("FnTable:IVRCompositor_0xx" returns a table of these, no this).
typedef void (*PFN_FnSetTrackingSpace)(ETrackingUniverseOrigin);
typedef EVRCompositorError (*PFN_FnWaitGetPoses)(TrackedDevicePose_t*, uint32_t, TrackedDevicePose_t*, uint32_t);
typedef EVRCompositorError (*PFN_FnSubmit)(EVREye, Texture_t*, VRTextureBounds_t*, EVRSubmitFlags);

// The mirror recorder (EyeCaptureMirror.exe) uses "FnTable:IVRCompositor_022" only, the table of openvr_capi.h's
// VR_IVRCompositor_FnTable for that version (41 entries; versions up to _027 only appended methods after it, _028 inserted
// SubmitWithArrayIndex at 6 -- so _022 is asked for explicitly, never the newest):
//    0 SetTrackingSpace  1 GetTrackingSpace  2 WaitGetPoses  3 GetLastPoses  4 GetLastPoseForTrackedDeviceIndex  5 Submit
//    6 ClearLastSubmittedFrame  7 PostPresentHandoff  8 GetFrameTiming  9 GetFrameTimings  10 GetFrameTimeRemaining
//   11 GetCumulativeStats  12 FadeToColor  13 GetCurrentFadeColor  14 FadeGrid  15 GetCurrentGridAlpha  16 SetSkyboxOverride
//   17 ClearSkyboxOverride  18 CompositorBringToFront  19 CompositorGoToBack  20 CompositorQuit  21 IsFullscreen
//   22 GetCurrentSceneFocusProcess  23 GetLastFrameRenderer  24 CanRenderScene  25 ShowMirrorWindow  26 HideMirrorWindow
//   27 IsMirrorWindowVisible  28 CompositorDumpImages  29 ShouldAppRenderWithLowResources  30 ForceInterleavedReprojectionOn
//   31 ForceReconnectProcess  32 SuspendRendering  33 GetMirrorTextureD3D11  34 ReleaseMirrorTextureD3D11  35 GetMirrorTextureGL
//   36 ReleaseSharedGLTexture  37 LockGLSharedTextureForAccess  38 UnlockGLSharedTextureForAccess
//   39 GetVulkanInstanceExtensionsRequired  40 GetVulkanDeviceExtensionsRequired
const uint32_t kFn022_GetCurrentSceneFocusProcess = 22;
const uint32_t kFn022_GetMirrorTextureD3D11 = 33;
const uint32_t kFn022_ReleaseMirrorTextureD3D11 = 34;
const uint32_t kFn022_Count = 41;
typedef uint32_t (*PFN_FnGetCurrentSceneFocusProcess)();
// pD3D11DeviceOrResource: the caller's ID3D11Device (first call) -> *ppD3D11ShaderResourceView = an SRV on that device of
// the eye's mirror texture (shared with the compositor; may be one texture for both eyes); released with
// ReleaseMirrorTextureD3D11.
typedef EVRCompositorError (*PFN_FnGetMirrorTextureD3D11)(EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView);
typedef void (*PFN_FnReleaseMirrorTextureD3D11)(void* pD3D11ShaderResourceView);

} // namespace vr

// Exports of openvr_api.dll (all extern "C"; VR_CALLTYPE = __cdecl, which is the one x64 convention).
extern "C" {
typedef uint32_t (*PFN_VR_InitInternal)(vr::EVRInitError* peError, vr::EVRApplicationType eType);
typedef uint32_t (*PFN_VR_InitInternal2)(vr::EVRInitError* peError, vr::EVRApplicationType eType, const char* pStartupInfo);
typedef void (*PFN_VR_ShutdownInternal)();
typedef void* (*PFN_VR_GetGenericInterface)(const char* pchInterfaceVersion, vr::EVRInitError* peError);
typedef bool (*PFN_VR_IsInterfaceVersionValid)(const char* pchInterfaceVersion);
typedef uint32_t (*PFN_VR_GetInitToken)();
typedef bool (*PFN_VR_IsHmdPresent)();
typedef bool (*PFN_VR_IsRuntimeInstalled)();
typedef const char* (*PFN_VR_RuntimePath)();
typedef bool (*PFN_VR_GetRuntimePath)(char* pchPathBuffer, uint32_t unBufferSize, uint32_t* punRequiredBufferSize);
typedef const char* (*PFN_VR_GetVRInitErrorAsSymbol)(vr::EVRInitError error);
typedef const char* (*PFN_VR_GetVRInitErrorAsEnglishDescription)(vr::EVRInitError error);
typedef const char* (*PFN_VR_GetStringForHmdError)(vr::EVRInitError error);
}
