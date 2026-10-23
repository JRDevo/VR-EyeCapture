// vr_hotkey -- see vr_hotkey.h.
#include "vr_hotkey.h"
#include <cstring>
#include <cwctype>

namespace {

const char* const kButtonNames[] = {"a", "b", "x", "y", "menu", "thumbstick", "trigger", "grip"};
const int kButtons = 8;

// One controller profile: per hand, the input component of each VrButton (index = (int)VrButton), nullptr = none.
struct ProfileDef {
    const char* path;
    const char* left[kButtons];
    const char* right[kButtons];
};
// order a, b, x, y, menu, thumbstick, trigger, grip
#define TOUCH_LEFT  {nullptr, nullptr, "x/click", "y/click", "menu/click", "thumbstick/click", "trigger/value", "squeeze/value"}
#define TOUCH_RIGHT {"a/click", "b/click", nullptr, nullptr, nullptr, "thumbstick/click", "trigger/value", "squeeze/value"}
const ProfileDef kProfiles[] = {
    // Oculus / Meta Touch family: the right "menu" is the reserved system (Meta) button -> no right_menu
    {"/interaction_profiles/oculus/touch_controller", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/meta/touch_controller_quest_2", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/meta/touch_controller_quest_1_rift_s", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/meta/touch_controller_rift_cv1", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/meta/touch_pro_controller", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/meta/touch_plus_controller", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/facebook/touch_controller_pro", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/meta/touch_controller_plus", TOUCH_LEFT, TOUCH_RIGHT},
    {"/interaction_profiles/khr/simple_controller",
     {nullptr, nullptr, nullptr, nullptr, "menu/click", nullptr, "select/click", nullptr},
     {nullptr, nullptr, nullptr, nullptr, "menu/click", nullptr, "select/click", nullptr}},
    {"/interaction_profiles/valve/index_controller",
     {"a/click", "b/click", nullptr, nullptr, nullptr, "thumbstick/click", "trigger/click", "squeeze/value"},
     {"a/click", "b/click", nullptr, nullptr, nullptr, "thumbstick/click", "trigger/click", "squeeze/value"}},
    // Vive wands: no stick -- "thumbstick" is the trackpad click
    {"/interaction_profiles/htc/vive_controller",
     {nullptr, nullptr, nullptr, nullptr, "menu/click", "trackpad/click", "trigger/click", "squeeze/click"},
     {nullptr, nullptr, nullptr, nullptr, "menu/click", "trackpad/click", "trigger/click", "squeeze/click"}},
    {"/interaction_profiles/microsoft/motion_controller",
     {nullptr, nullptr, nullptr, nullptr, "menu/click", "thumbstick/click", "trigger/value", "squeeze/click"},
     {nullptr, nullptr, nullptr, nullptr, "menu/click", "thumbstick/click", "trigger/value", "squeeze/click"}},
    {"/interaction_profiles/samsung/odyssey_controller",
     {nullptr, nullptr, nullptr, nullptr, "menu/click", "thumbstick/click", "trigger/value", "squeeze/click"},
     {nullptr, nullptr, nullptr, nullptr, "menu/click", "thumbstick/click", "trigger/value", "squeeze/click"}},
    {"/interaction_profiles/hp/mixed_reality_controller",
     {nullptr, nullptr, "x/click", "y/click", "menu/click", "thumbstick/click", "trigger/value", "squeeze/value"},
     {"a/click", "b/click", nullptr, nullptr, "menu/click", "thumbstick/click", "trigger/value", "squeeze/value"}},
    {"/interaction_profiles/bytedance/pico4_controller",
     {nullptr, nullptr, "x/click", "y/click", "menu/click", "thumbstick/click", "trigger/value", "squeeze/value"},
     {"a/click", "b/click", nullptr, nullptr, nullptr, "thumbstick/click", "trigger/value", "squeeze/value"}},
    {"/interaction_profiles/bytedance/pico_neo3_controller",
     {nullptr, nullptr, "x/click", "y/click", "menu/click", "thumbstick/click", "trigger/value", "squeeze/value"},
     {"a/click", "b/click", nullptr, nullptr, "menu/click", "thumbstick/click", "trigger/value", "squeeze/value"}},
    {"/interaction_profiles/htc/vive_cosmos_controller",
     {nullptr, nullptr, "x/click", "y/click", "menu/click", "thumbstick/click", "trigger/value", "squeeze/click"},
     {"a/click", "b/click", nullptr, nullptr, nullptr, "thumbstick/click", "trigger/value", "squeeze/click"}},
    {"/interaction_profiles/htc/vive_focus3_controller",
     {nullptr, nullptr, "x/click", "y/click", "menu/click", "thumbstick/click", "trigger/value", "squeeze/click"},
     {"a/click", "b/click", nullptr, nullptr, nullptr, "thumbstick/click", "trigger/value", "squeeze/click"}},
};
#undef TOUCH_LEFT
#undef TOUCH_RIGHT

std::wstring Squeeze(const std::wstring& s) {   // lower case, no white space
    std::wstring r;
    for (wchar_t c : s) if (!iswspace(c)) r += (wchar_t)towlower(c);
    return r;
}

bool ParseButton(const std::wstring& t, VrButtonRef& b) {
    std::wstring rest;
    if (t.compare(0, 5, L"left_") == 0) { b.hand = VrHand::Left; rest = t.substr(5); }
    else if (t.compare(0, 6, L"right_") == 0) { b.hand = VrHand::Right; rest = t.substr(6); }
    else return false;
    if (rest == L"squeeze") rest = L"grip";   // the OpenXR name, accepted as an alias
    if (rest == L"stick") rest = L"thumbstick";
    for (int i = 0; i < kButtons; ++i) {
        std::wstring n;
        for (const char* p = kButtonNames[i]; *p; ++p) n += (wchar_t)*p;
        if (rest == n) { b.button = (VrButton)i; return true; }
    }
    return false;
}

} // namespace

VrHotkey DefaultVrHotkey() {
    VrHotkey h;
    ParseVrHotkey(kDefaultVrHotkey, h);
    return h;
}

bool ParseVrHotkey(const std::wstring& text, VrHotkey& out) {
    const std::wstring s = Squeeze(text);
    VrHotkey h;
    if (s == L"off" || s == L"none" || s == L"0") {
        h.enabled = false;
        h.text = L"off";
        out = h;
        return true;
    }
    if (s == L"thumbsticks" || s == L"sticks") {
        h.buttons = {{VrHand::Left, VrButton::Thumbstick}, {VrHand::Right, VrButton::Thumbstick}};
        h.text = L"thumbsticks";
        out = h;
        return true;
    }
    if (s.empty()) return false;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t plus = s.find(L'+', pos);
        if (plus == std::wstring::npos) plus = s.size();
        VrButtonRef b;
        if (!ParseButton(s.substr(pos, plus - pos), b)) return false;
        for (const VrButtonRef& e : h.buttons) if (e == b) return false;   // the same button twice
        h.buttons.push_back(b);
        if ((int)h.buttons.size() > kVrHotkeyMaxButtons) return false;
        pos = plus + 1;
    }
    if (h.buttons.empty()) return false;
    const bool sticks = h.buttons.size() == 2 && h.buttons[0].button == VrButton::Thumbstick &&
                        h.buttons[1].button == VrButton::Thumbstick && h.buttons[0].hand != h.buttons[1].hand;
    if (sticks) {
        h.buttons = {{VrHand::Left, VrButton::Thumbstick}, {VrHand::Right, VrButton::Thumbstick}};
        h.text = L"thumbsticks";
    } else {
        for (size_t i = 0; i < h.buttons.size(); ++i) {
            if (i) h.text += L'+';
            for (char c : VrButtonName(h.buttons[i])) h.text += (wchar_t)c;
        }
    }
    out = h;
    return true;
}

std::string VrButtonName(const VrButtonRef& b) {
    std::string s = b.hand == VrHand::Left ? "left_" : "right_";
    const int i = (int)b.button;
    s += i >= 0 && i < kButtons ? kButtonNames[i] : "?";
    return s;
}

const std::vector<const char*>& VrKnownProfiles() {
    static const std::vector<const char*>* v = [] {
        auto* p = new std::vector<const char*>;
        for (const ProfileDef& d : kProfiles) p->push_back(d.path);
        return p;
    }();
    return *v;
}

bool VrButtonPath(const std::string& profile, const VrButtonRef& b, std::string& path) {
    for (const ProfileDef& d : kProfiles) {
        if (profile != d.path) continue;
        const int i = (int)b.button;
        if (i < 0 || i >= kButtons) return false;
        const char* c = b.hand == VrHand::Left ? d.left[i] : d.right[i];
        if (!c) return false;
        path = std::string(b.hand == VrHand::Left ? "/user/hand/left/input/" : "/user/hand/right/input/") + c;
        return true;
    }
    return false;
}

bool VrSameInput(const std::string& appPath, const std::string& ourPath) {
    // our paths are <top>/input/<identifier>/<component>; the identifier part is what both read
    const size_t in = ourPath.find("/input/");
    if (in == std::string::npos) return appPath == ourPath;
    const size_t slash = ourPath.find('/', in + 7);
    const std::string id = slash == std::string::npos ? ourPath : ourPath.substr(0, slash);
    return appPath == id || (appPath.size() > id.size() && appPath.compare(0, id.size(), id) == 0 && appPath[id.size()] == '/');
}

const std::vector<const char*>& VrSimpleTranslationTargets() {
    static const std::vector<const char*> t = {"/interaction_profiles/oculus/touch_controller", "/interaction_profiles/valve/index_controller",
                                               "/interaction_profiles/htc/vive_controller", "/interaction_profiles/microsoft/motion_controller",
                                               "/interaction_profiles/hp/mixed_reality_controller", "/interaction_profiles/bytedance/pico4_controller"};
    return t;
}

bool VrTranslateSimpleBinding(const std::string& target, const std::string& p, std::string& out) {
    std::string hand;
    if (p.compare(0, 16, "/user/hand/left/") == 0) hand = "left";
    else if (p.compare(0, 17, "/user/hand/right/") == 0) hand = "right";
    else return false;
    const std::string top = "/user/hand/" + hand + "/", rest = p.substr(top.size());
    if (rest == "input/grip/pose" || rest == "input/aim/pose" || rest == "output/haptic") { out = p; return true; }
    const bool touch = target == "/interaction_profiles/oculus/touch_controller", index = target == "/interaction_profiles/valve/index_controller";
    const bool vive = target == "/interaction_profiles/htc/vive_controller", pico = target == "/interaction_profiles/bytedance/pico4_controller";
    if (rest == "input/select/click" || rest == "input/select") {
        out = top + ((index || vive) ? "input/trigger/click" : "input/trigger/value");
        return true;
    }
    if (rest == "input/menu/click" || rest == "input/menu") {
        if (index) { out = top + "input/b/click"; return true; }
        if ((touch || pico) && hand == "right") return false;   // the right "menu" is the system button there
        out = top + "input/menu/click";
        return true;
    }
    return false;
}

bool VrHoldTracker::Update(bool allDown, double now, double holdS) {
    if (!allDown) { since_ = -1; fired_ = false; return false; }
    if (since_ < 0) since_ = now;
    if (!fired_ && now - since_ >= holdS) { fired_ = true; return true; }
    return false;
}
