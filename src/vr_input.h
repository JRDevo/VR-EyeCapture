// vr_input -- the VR controller hotkey through OpenXR actions (vr_hotkey, src/vr_hotkey.h): the layer adds its OWN action set
// to the game's input, so it works in any OpenXR game (64-bit and the 32-bit layer), whatever the game does with the keyboard.
//
// How it stays out of the game's way (OpenXR 1.0 / 1.1 rules):
//   * xrSuggestInteractionProfileBindings REPLACES the previous suggestion for the same profile of the same instance, and
//     the runtime picks ONE interaction profile per hand from all suggested profiles. So the layer (a) forwards the game's
//     suggestions untouched and remembers them, (b) only when the game attaches its action sets (xrAttachSessionActionSets:
//     the last moment suggestions are allowed) re-suggests each profile the game suggested as the game's bindings + ours,
//     and (c) never adds a profile the game did not suggest (that could make the runtime switch the game to another
//     profile). A merged suggestion the runtime refuses is followed by the game's own suggestion again (restored).
//     Only a game that suggests no profile at all gets every profile of ours.
//   * Our action set's priority = the highest priority of the game's action sets that bind one of our inputs (0 if none
//     does): OpenXR ignores an input in every action set of LOWER priority than the highest one using it, so equal
//     priority means both the game and we see the button; never higher (we would take it from the game).
//   * xrAttachSessionActionSets: our set is appended to the game's list (once per session, as the game calls it).
//     xrSyncActions: our set is appended to the game's active sets. Any error that our addition causes: the call is
//     repeated exactly as the game made it and the controller hotkey is off for that session (logged); the game always
//     gets the result of its own call.
//   * A game that never creates an action set (no controller input at all) gets ours attached by the layer itself after
//     20 s of frames (xrEndFrame; test override XR_EYE_CAPTURE_TEST_VR_SELF_ATTACH_S), synced by the layer every frame.
//   * Haptics: a short pulse on both controllers when a recording is started, a longer one when it is stopped.
// The layer calls the next layer's functions directly; our hooks never see our own calls. Dormant sessions (no tray) are
// never touched.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include "vr_hotkey.h"

struct VrInputDispatch {
    PFN_xrStringToPath                    StringToPath = nullptr;
    PFN_xrPathToString                    PathToString = nullptr;
    PFN_xrCreateActionSet                 CreateActionSet = nullptr;
    PFN_xrCreateAction                    CreateAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets         AttachSessionActionSets = nullptr;
    PFN_xrSyncActions                     SyncActions = nullptr;
    PFN_xrGetActionStateBoolean           GetActionStateBoolean = nullptr;
    PFN_xrApplyHapticFeedback             ApplyHapticFeedback = nullptr;
    PFN_xrGetCurrentInteractionProfile    GetCurrentInteractionProfile = nullptr;
    bool Complete() const;
};

// The combination was held: 1 = a recording start was requested, 2 = a stop was requested, 0 = nothing happened.
using VrActivateFn = std::function<int(XrSession)>;

class VrInput {   // one per XrInstance
public:
    VrInput(XrInstance instance, const VrInputDispatch& d, const VrHotkey& hk, double holdS, VrActivateFn activate);
    bool Enabled() const { return enabled_; }

    // A session of this instance was created / destroyed. armed = not dormant (only armed sessions are touched).
    void SessionCreated(XrSession s, bool armed);
    void SessionDestroyed(XrSession s);

    // After the game's call succeeded (bookkeeping only).
    void ActionSetCreated(XrActionSet set, const XrActionSetCreateInfo* ci);
    void ActionCreated(XrAction action, XrActionSet set);
    void Suggested(const XrInteractionProfileSuggestedBinding* sb);
    // These forward the game's call themselves (with our set added) and return what the game must see.
    XrResult Attach(XrSession s, const XrSessionActionSetsAttachInfo* ai);
    XrResult Sync(XrSession s, const XrActionsSyncInfo* si);
    // Game thread, inside xrEndFrame before it is forwarded: the self-attach / self-sync of games without input.
    void EndFrame(XrSession s);

private:
    struct SessionState {
        bool   armed = false;
        bool   appAttach = false;     // the game called xrAttachSessionActionSets
        bool   ours = false;          // our set is attached to this session
        bool   self = false;          // ... by us (the game has no input of its own): we sync it ourselves
        bool   off = false;           // our addition caused an error: the hotkey is off for this session
        bool   loggedOff = false;
        double firstFrame = -1;
        uint64_t syncs = 0;
        VrHoldTracker hold;
        double lastProfileCheck = -1;
        std::string profile[2];       // current interaction profile per hand (logged when it changes)
        int    profileLogs = 0;
        bool   loggedFirstSync = false;
    };
    bool PrepareLocked(const std::vector<XrActionSet>& appSets);   // our set, actions, suggestions (first attach of the instance)
    void Evaluate(XrSession s, bool focused);
    std::string PathString(XrPath p);
    XrPath Path(const std::string& s);
    void Haptic(XrSession s, int kind);

    XrInstance inst_;
    VrInputDispatch d_;
    VrHotkey hk_;
    double holdS_;
    VrActivateFn activate_;
    bool enabled_ = true;
    std::mutex m_;
    // the game's input (this instance)
    std::map<XrActionSet, uint32_t> appSets_;              // priority
    std::map<XrAction, XrActionSet> appActions_;
    std::map<XrPath, std::vector<XrActionSuggestedBinding>> appSuggest_;   // the game's latest suggestion per profile
    std::map<XrPath, std::string> pathNames_;
    bool frozen_ = false;          // some session of the instance has attached: no more suggestions
    bool prepared_ = false, prepareFailed_ = false;
    bool loggedLateSet_ = false;
    // ours
    XrActionSet set_ = XR_NULL_HANDLE;
    std::vector<XrAction> buttons_;
    XrAction haptic_ = XR_NULL_HANDLE;
    std::string ownSummary_;
    std::map<XrSession, SessionState> sessions_;
    double selfAttachS_ = 20.0;
};

double VrNowS();   // monotonic seconds (QPC)
