// input -- the tray's game-controller hotkeys: XInput pads (Pad: bindings) and DirectInput 8 game controllers such as
// wheels, pedals, shifters, button boxes and HOTAS (Joy: bindings). See src/hotkey.h for the binding syntax.
//
// One poll thread (20 ms) owns everything: XInput is loaded at run time (xinput1_4 -> xinput1_3 -> xinput9_1_0, Guide via
// the hidden XInputGetStateEx = ordinal 100 when present); DirectInput enumerates attached DI8DEVCLASS_GAMECTRL devices,
// skips XInput devices (DIPROP_GUIDANDPATH contains "IG_": they are read as pads) and reads c_dfDIJoystick2 in
// background / non-exclusive mode through a hidden top-level window that also receives WM_DEVICECHANGE (re-enumerated
// 1 s after the last device change, and every 10 s while a Joy binding's device is missing). A device whose reads keep
// failing is dropped until the next enumeration. Keyboard / mouse bindings are NOT handled here (the layer polls them).
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "hotkey.h"

namespace input {

struct DeviceInfo {
    std::wstring kind;            // "pad" (XInput) or "joy" (DirectInput)
    std::wstring name;            // pad: "XInput pad <n>"; joy: the sanitised product name a Joy: binding matches
    unsigned index = 0;           // pad: XInput slot 1..4; joy: enumeration order 1..n
    unsigned buttons = 0;         // pad: nameable buttons (17); joy: DIDEVCAPS::dwButtons
    unsigned povs = 0;            // joy: POV hats (only the first one is read)
    bool xinputSkipped = false;   // joy: an XInput device DirectInput also lists; it is read as a pad instead
};

// Starts the poll thread (no-op when running). onFire runs ON THE POLL THREAD when a Pad / Joy binding is pressed
// (rising edge of "all its buttons held", 300 ms debounce per binding); it may be empty.
void Start(std::function<void(const HotkeyBinding&)> onFire);
void Stop();
// Waits until the first device enumeration finished (true) or the timeout passed. For --inputs.
bool WaitReady(unsigned timeoutMs);
// Replaces the bindings; keeps only the Pad / Joy kinds. Thread-safe; may be called before Start.
void SetBindings(const std::vector<HotkeyBinding>& all);
// Settings "Press a key...": while on, no binding fires and pad / joystick presses are captured instead. Buttons already
// held when capture starts are ignored until released.
void SetCapture(bool on);
// The chord text ("Pad:A", "Joy:<name>:Button3+Button4") once the user pressed and RELEASED pad / joystick buttons: the
// union of the buttons held just before the first release, on ONE device (presses on other devices are ignored). Once.
bool TakeCaptured(std::wstring& text);
// Snapshot of the connected pads and DirectInput devices (XInput-skipped ones included, flagged).
std::vector<DeviceInfo> Devices();
// One line for logs: "XInput pad 1 (17 buttons); "T300RS..." (13 buttons, 1 POV); ..." or "none".
std::wstring DevicesText(const std::vector<DeviceInfo>& devices);

} // namespace input
