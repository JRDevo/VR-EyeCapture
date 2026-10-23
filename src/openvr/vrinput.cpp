// vrinput -- see vrinput.h.
#include "vrinput.h"

namespace {

const char* HandName(VrHand h) { return h == VrHand::Left ? "left" : "right"; }

std::string Src(VrHand h, const char* component) { return std::string("/user/hand/") + HandName(h) + "/input/" + component; }

void Esc(std::string& out, const std::string& s) {
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
}

}  // namespace

const std::vector<const char*>& SteamVrControllerTypes() {
    // Quest / Rift / Pico and most streamers (Virtual Desktop, Steam Link, ALVR) report oculus_touch; Index = knuckles;
    // Vive wands; WMR (Odyssey, Reverb G1); HP Reverb G2; Vive Cosmos. Any other controller: SteamVR -> Controller Bindings.
    static const std::vector<const char*> t = {"oculus_touch", "knuckles", "vive_controller", "holographic_controller",
                                               "hpmotioncontroller", "vive_cosmos_controller"};
    return t;
}

bool SteamVrButtonSource(const std::string& type, const VrButtonRef& b, SteamVrSource& out) {
    const bool left = b.hand == VrHand::Left;
    auto set = [&](const char* component, const char* mode, const char* input) {
        out.path = Src(b.hand, component);
        out.mode = mode;
        out.input = input;
        return true;
    };
    if (type == "oculus_touch") {   // Quest / Rift: x y on the left, a b on the right; the menu button is not offered to apps
        switch (b.button) {
        case VrButton::A: return !left && set("a", "button", "click");
        case VrButton::B: return !left && set("b", "button", "click");
        case VrButton::X: return left && set("x", "button", "click");
        case VrButton::Y: return left && set("y", "button", "click");
        case VrButton::Thumbstick: return set("joystick", "joystick", "click");
        case VrButton::Trigger: return set("trigger", "trigger", "click");
        case VrButton::Grip: return set("grip", "trigger", "click");
        case VrButton::Menu: return false;
        }
    } else if (type == "knuckles") {   // Valve Index: a b on both hands, no x y / menu
        switch (b.button) {
        case VrButton::A: return set("a", "button", "click");
        case VrButton::B: return set("b", "button", "click");
        case VrButton::Thumbstick: return set("thumbstick", "joystick", "click");
        case VrButton::Trigger: return set("trigger", "trigger", "click");
        case VrButton::Grip: return set("grip", "grab", "grab");
        default: return false;
        }
    } else if (type == "vive_controller") {   // Vive wands: the trackpad click stands in for the stick
        switch (b.button) {
        case VrButton::Menu: return set("application_menu", "button", "click");
        case VrButton::Thumbstick: return set("trackpad", "trackpad", "click");
        case VrButton::Trigger: return set("trigger", "trigger", "click");
        case VrButton::Grip: return set("grip", "button", "click");
        default: return false;
        }
    } else if (type == "hpmotioncontroller" || type == "vive_cosmos_controller") {
        // HP Reverb G2 / Vive Cosmos: x y on the left, a b on the right, a menu button on the left
        switch (b.button) {
        case VrButton::A: return !left && set("a", "button", "click");
        case VrButton::B: return !left && set("b", "button", "click");
        case VrButton::X: return left && set("x", "button", "click");
        case VrButton::Y: return left && set("y", "button", "click");
        case VrButton::Menu: return left && set("application_menu", "button", "click");
        case VrButton::Thumbstick: return set("joystick", "joystick", "click");
        case VrButton::Trigger: return set("trigger", "trigger", "click");
        case VrButton::Grip: return set("grip", "trigger", "click");
        }
    } else if (type == "holographic_controller") {   // Windows Mixed Reality
        switch (b.button) {
        case VrButton::Menu: return set("application_menu", "button", "click");
        case VrButton::Thumbstick: return set("joystick", "joystick", "click");
        case VrButton::Trigger: return set("trigger", "trigger", "click");
        case VrButton::Grip: return set("grip", "button", "click");
        default: return false;
        }
    }
    return false;
}

int SteamVrLegacyButtonId(const VrButtonRef& b) {
    switch (b.button) {   // EVRButtonId: ApplicationMenu 1, Grip 2, A 7, Axis0 32 (stick / trackpad click), Axis1 33 (trigger)
    case VrButton::A: case VrButton::X: return 7;
    case VrButton::B: case VrButton::Y: case VrButton::Menu: return 1;
    case VrButton::Grip: return 2;
    case VrButton::Thumbstick: return 32;
    case VrButton::Trigger: return 33;
    }
    return -1;
}

std::string SteamVrButtonAction(int i) { return std::string(kSteamVrActionSet) + "/in/button" + std::to_string(i + 1); }

std::string SteamVrManifestJson(const VrHotkey& hk) {
    std::string j = "{\n  \"default_bindings\": [\n";
    const auto& types = SteamVrControllerTypes();
    for (size_t i = 0; i < types.size(); ++i) {
        j += "    { \"controller_type\": \"" + std::string(types[i]) + "\", \"binding_url\": \"bindings_" + types[i] + ".json\" }";
        j += i + 1 < types.size() ? ",\n" : "\n";
    }
    j += "  ],\n  \"actions\": [\n";
    for (size_t i = 0; i < hk.buttons.size(); ++i)
        j += "    { \"name\": \"" + SteamVrButtonAction((int)i) + "\", \"type\": \"boolean\", \"requirement\": \"optional\" },\n";
    j += "    { \"name\": \"" + std::string(kSteamVrHapticAction) + "\", \"type\": \"vibration\", \"requirement\": \"optional\" }\n";
    j += "  ],\n  \"action_sets\": [ { \"name\": \"" + std::string(kSteamVrActionSet) + "\", \"usage\": \"leftright\" } ],\n";
    j += "  \"localization\": [ { \"language_tag\": \"en_US\", \"" + std::string(kSteamVrActionSet) + "\": \"VR EyeCapture\"";
    for (size_t i = 0; i < hk.buttons.size(); ++i) {
        j += ", \"" + SteamVrButtonAction((int)i) + "\": \"Record start / stop: hold ";
        Esc(j, VrButtonName(hk.buttons[i]));
        j += "\"";
    }
    j += ", \"" + std::string(kSteamVrHapticAction) + "\": \"Record start / stop vibration\" } ]\n}\n";
    return j;
}

std::string SteamVrBindingJson(const std::string& type, const VrHotkey& hk, int* bound) {
    int n = 0;
    std::string j = "{\n  \"controller_type\": \"" + type + "\",\n  \"name\": \"VR EyeCapture default\",\n"
                    "  \"description\": \"Hold ";
    std::string text;
    for (wchar_t c : hk.text) text += (char)(c < 0x80 ? c : L'?');   // ASCII (ParseVrHotkey's canonical text)
    Esc(j, text);
    j += " to start / stop recording\",\n  \"bindings\": {\n    \"" + std::string(kSteamVrActionSet) + "\": {\n      \"sources\": [";
    for (size_t i = 0; i < hk.buttons.size(); ++i) {
        SteamVrSource s;
        if (!SteamVrButtonSource(type, hk.buttons[i], s)) continue;
        j += n ? ",\n" : "\n";
        ++n;
        j += "        { \"path\": \"" + s.path + "\", \"mode\": \"" + s.mode + "\", \"inputs\": { \"" + s.input +
             "\": { \"output\": \"" + SteamVrButtonAction((int)i) + "\" } } }";
    }
    j += "\n      ],\n      \"haptics\": [\n";
    j += "        { \"output\": \"" + std::string(kSteamVrHapticAction) + "\", \"path\": \"/user/hand/left/output/haptic\" },\n";
    j += "        { \"output\": \"" + std::string(kSteamVrHapticAction) + "\", \"path\": \"/user/hand/right/output/haptic\" }\n";
    j += "      ]\n    }\n  }\n}\n";
    if (bound) *bound = n;
    return j;
}
