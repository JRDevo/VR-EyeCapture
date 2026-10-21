// vr_hotkey -- the VR controller hotkey (config vr_hotkey / vr_hotkey_hold_s): the button-combination grammar, which
// OpenXR input path each button has on each controller profile, and the hold detector. Pure logic, no OpenXR calls
// (src/vr_input.cpp does those); unit-tested by tests/vr_hotkey_test.cpp.
//
// Grammar (case-insensitive, spaces ignored):
//   thumbsticks            = left_thumbstick+right_thumbstick (the default: both stick clicks, rare in games)
//   off                    = no controller hotkey (the layer adds no action set at all)
//   <hand>_<button>[+...]  1 to 4 distinct buttons, all held together for vr_hotkey_hold_s seconds
//     hand   = left | right
//     button = a | b | x | y | menu | thumbstick | trigger | grip
// Example: left_menu+right_a. A button a profile does not have (right_menu on Oculus Touch: the right "menu" is the
// reserved system button) makes the combination unavailable on THAT profile only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class VrHand : uint8_t { Left, Right };
enum class VrButton : uint8_t { A, B, X, Y, Menu, Thumbstick, Trigger, Grip };
struct VrButtonRef {
    VrHand   hand = VrHand::Left;
    VrButton button = VrButton::Thumbstick;
    bool operator==(const VrButtonRef& o) const { return hand == o.hand && button == o.button; }
};

const int kVrHotkeyMaxButtons = 4;
constexpr const wchar_t kDefaultVrHotkey[] = L"thumbsticks";
constexpr float kDefaultVrHotkeyHoldS = 1.0f;

struct VrHotkey {
    bool                     enabled = true;
    std::vector<VrButtonRef> buttons;   // 1..4, distinct (empty when off)
    std::wstring             text;      // canonical text: "thumbsticks", "off" or "left_menu+right_a"
};

// false = bad text (out unchanged). The canonical text of the default combination is "thumbsticks".
bool ParseVrHotkey(const std::wstring& text, VrHotkey& out);
VrHotkey DefaultVrHotkey();
std::string VrButtonName(const VrButtonRef& b);   // "left_thumbstick"

// The interaction profiles the layer knows (OpenXR paths, "/interaction_profiles/oculus/touch_controller", ...).
const std::vector<const char*>& VrKnownProfiles();
// The input path of `b` on `profile`, e.g. "/user/hand/left/input/thumbstick/click"; false = the profile has no such
// button (or the profile is unknown).
bool VrButtonPath(const std::string& profile, const VrButtonRef& b, std::string& path);
// Whether `appPath` (a binding path of the game) reads the same input as our `ourPath`: the same component
// ("/user/hand/left/input/thumbstick" with or without "/click", "/value", ...). Used for the action set priority.
bool VrSameInput(const std::string& appPath, const std::string& ourPath);

// Hold detector: Update() once per evaluated input frame; returns true once when every button has been held
// together for holdS seconds; it re-arms only after a button was released.
class VrHoldTracker {
public:
    bool Update(bool allDown, double now, double holdS);
    bool Holding() const { return since_ >= 0; }
    void Reset() { since_ = -1; fired_ = false; }
private:
    double since_ = -1;
    bool   fired_ = false;
};
