// vrinput -- the VR controller hotkey for SteamVR games (EyeCaptureVrInput.exe, src/openvr/vrinput_main.cpp): which SteamVR
// input source each vr_hotkey button is on each controller type, the action manifest + default bindings the helper hands to
// SteamVR, and the legacy button bit of each button (the fallback for a SteamVR that gives the helper no action data).
// Pure logic, no OpenVR calls; unit-tested by tests/openvr_hook_test.cpp.
//
// Why a helper process: SteamVR Input takes ONE action manifest per process, and a game registers its own -- the proxy inside
// the game cannot add actions without rewriting the game's manifest and bindings. The helper is its own SteamVR overlay
// application ("VR EyeCapture" in SteamVR's controller bindings) with this action set:
//   /actions/eyecapture/in/button1 .. button4   boolean, one per vr_hotkey button (button i = hk.buttons[i])
//   /actions/eyecapture/out/haptic              vibration on both hands (a short pulse on start / stop)
#pragma once
#include <string>
#include <vector>
#include "vr_hotkey.h"

constexpr const char kSteamVrActionSet[] = "/actions/eyecapture";
constexpr const char kSteamVrHapticAction[] = "/actions/eyecapture/out/haptic";

// One binding source: {"path": path, "mode": mode, "inputs": {input: {"output": action}}}.
struct SteamVrSource {
    std::string path;    // "/user/hand/left/input/joystick"
    const char* mode;    // "joystick", "button", "trigger", "trackpad", "grab"
    const char* input;   // "click", "grab"
};

// The controller types the default bindings cover (SteamVR controller_type names).
const std::vector<const char*>& SteamVrControllerTypes();
// The source of `b` on controller `type`; false = the controller has no such button (or the type is unknown).
bool SteamVrButtonSource(const std::string& type, const VrButtonRef& b, SteamVrSource& out);
// The legacy IVRSystem::GetControllerState button id of `b` (ulButtonPressed bit), -1 = none.
int SteamVrLegacyButtonId(const VrButtonRef& b);
// "/actions/eyecapture/in/button1" for i = 0.
std::string SteamVrButtonAction(int i);

// The JSON files (UTF-8): the manifest names one binding file per controller type ("bindings_<type>.json"); a binding file
// binds every button the controller has (a controller missing one cannot do that combination: `bound` < buttons).
std::string SteamVrManifestJson(const VrHotkey& hk);
std::string SteamVrBindingJson(const std::string& type, const VrHotkey& hk, int* bound);
