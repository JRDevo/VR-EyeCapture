// vr_input -- see vr_input.h.
#include "vr_input.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "log.h"

namespace {
const char* const kSetName = "eyecapture_vr_hotkey";
const char* const kSetLocalized = "VR EyeCapture controller hotkey";

std::string ShortProfile(const std::string& p) {   // "/interaction_profiles/oculus/touch_controller" -> "oculus/touch_controller"
    const char* pre = "/interaction_profiles/";
    return p.compare(0, strlen(pre), pre) == 0 ? p.substr(strlen(pre)) : p;
}
} // namespace

double VrNowS() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

bool VrInputDispatch::Complete() const {
    return StringToPath && PathToString && CreateActionSet && CreateAction && SuggestInteractionProfileBindings &&
           AttachSessionActionSets && SyncActions && GetActionStateBoolean && ApplyHapticFeedback && GetCurrentInteractionProfile;
}

VrInput::VrInput(XrInstance instance, const VrInputDispatch& d, const VrHotkey& hk, double holdS, VrActivateFn activate)
    : inst_(instance), d_(d), hk_(hk), holdS_(holdS), activate_(std::move(activate)) {
    enabled_ = hk_.enabled && !hk_.buttons.empty();
    if (enabled_ && !d_.Complete()) {
        LOGW("vr hotkey: the next layer / runtime did not return the action functions -- controller hotkey off");
        enabled_ = false;
    }
    wchar_t v[32];
    if (GetEnvironmentVariableW(L"XR_EYE_CAPTURE_TEST_VR_SELF_ATTACH_S", v, 32) > 0) {
        selfAttachS_ = _wtof(v);
        LOGI("vr hotkey: XR_EYE_CAPTURE_TEST_VR_SELF_ATTACH_S=%.1f (test): a game without input gets our action set after %.1f s",
             selfAttachS_, selfAttachS_);
    }
}

std::string VrInput::PathString(XrPath p) {
    if (p == XR_NULL_PATH) return std::string();
    auto it = pathNames_.find(p);
    if (it != pathNames_.end()) return it->second;
    char buf[XR_MAX_PATH_LENGTH] = "";
    uint32_t n = 0;
    std::string s;
    if (XR_SUCCEEDED(d_.PathToString(inst_, p, (uint32_t)sizeof(buf), &n, buf))) s = buf;
    pathNames_[p] = s;
    return s;
}

XrPath VrInput::Path(const std::string& s) {
    XrPath p = XR_NULL_PATH;
    if (XR_FAILED(d_.StringToPath(inst_, s.c_str(), &p))) return XR_NULL_PATH;
    pathNames_[p] = s;
    return p;
}

void VrInput::SessionCreated(XrSession s, bool armed) {
    std::lock_guard<std::mutex> lk(m_);
    sessions_[s].armed = armed;
}

void VrInput::SessionDestroyed(XrSession s) {
    std::lock_guard<std::mutex> lk(m_);
    sessions_.erase(s);
}

void VrInput::ActionSetCreated(XrActionSet set, const XrActionSetCreateInfo* ci) {
    std::lock_guard<std::mutex> lk(m_);
    appSets_[set] = ci ? ci->priority : 0;
    bool self = false;
    for (auto& kv : sessions_) self = self || kv.second.self;
    if (self && !loggedLateSet_) {
        loggedLateSet_ = true;
        LOGE("vr hotkey: the game creates its own action set ('%s') AFTER the layer attached its controller hotkey to a game "
             "without input -- the game's own controller input will fail to attach; set vr_hotkey = off in config.ini for "
             "this game", ci ? ci->actionSetName : "?");
    }
}

void VrInput::ActionCreated(XrAction action, XrActionSet set) {
    std::lock_guard<std::mutex> lk(m_);
    appActions_[action] = set;
}

void VrInput::Suggested(const XrInteractionProfileSuggestedBinding* sb) {
    if (!sb) return;
    std::lock_guard<std::mutex> lk(m_);
    std::vector<XrActionSuggestedBinding>& v = appSuggest_[sb->interactionProfile];
    v.assign(sb->suggestedBindings, sb->suggestedBindings + (sb->suggestedBindings ? sb->countSuggestedBindings : 0));
    // a profile the game suggests again after our merge (its attach failed and it retries): ours is re-merged at the next attach
    if (prepared_ && !frozen_) prepared_ = false;
}

bool VrInput::PrepareLocked(const std::vector<XrActionSet>& appSets) {
    if (prepared_) return true;
    if (prepareFailed_) return false;
    // ---- our action set + actions (once per instance)
    if (set_ == XR_NULL_HANDLE) {
        prepareFailed_ = true;   // cleared below
        // priority: the highest of the game's sets that bind one of our inputs (equal = both see it; never higher)
        uint32_t prio = 0;
        std::string prioFrom;
        for (const auto& kv : appSuggest_) {
            const std::string prof = PathString(kv.first);
            std::vector<std::string> ours;
            for (const VrButtonRef& b : hk_.buttons) {
                std::string p;
                if (VrButtonPath(prof, b, p)) ours.push_back(p);
            }
            for (const XrActionSuggestedBinding& sb : kv.second) {
                const std::string bp = PathString(sb.binding);
                for (const std::string& op : ours) {
                    if (!VrSameInput(bp, op)) continue;
                    auto a = appActions_.find(sb.action);
                    auto st = a == appActions_.end() ? appSets_.end() : appSets_.find(a->second);
                    if (st != appSets_.end() && st->second >= prio) {
                        if (st->second > prio || prioFrom.empty()) prioFrom = ShortProfile(prof) + " " + bp;
                        prio = st->second;
                    }
                }
            }
        }
        uint32_t maxApp = 0;
        for (XrActionSet s : appSets) { auto it = appSets_.find(s); if (it != appSets_.end() && it->second > maxApp) maxApp = it->second; }
        XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(ci.actionSetName, kSetName);
        strcpy_s(ci.localizedActionSetName, kSetLocalized);
        ci.priority = prio;
        XrResult r = d_.CreateActionSet(inst_, &ci, &set_);
        if (XR_FAILED(r)) {
            set_ = XR_NULL_HANDLE;
            LOGW("vr hotkey: xrCreateActionSet failed (%d) -- controller hotkey off", (int)r);
            return false;
        }
        LOGI("vr hotkey: action set '%s' priority %u (%s; the game's attached sets go up to priority %u)", kSetName, prio,
             prioFrom.empty() ? "no game action reads our buttons" : ("same as the game's action on " + prioFrom).c_str(), maxApp);
        for (size_t i = 0; i < hk_.buttons.size(); ++i) {
            XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
            ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            snprintf(ai.actionName, sizeof(ai.actionName), "button_%u", (unsigned)i + 1);
            snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "VR EyeCapture button %u (%s)", (unsigned)i + 1,
                     VrButtonName(hk_.buttons[i]).c_str());
            XrAction a = XR_NULL_HANDLE;
            r = d_.CreateAction(set_, &ai, &a);
            if (XR_FAILED(r)) { LOGW("vr hotkey: xrCreateAction(%s) failed (%d) -- controller hotkey off", ai.actionName, (int)r); return false; }
            buttons_.push_back(a);
        }
        XrActionCreateInfo hi{XR_TYPE_ACTION_CREATE_INFO};
        hi.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        strcpy_s(hi.actionName, "haptic");
        strcpy_s(hi.localizedActionName, "VR EyeCapture haptic");
        if (XR_FAILED(d_.CreateAction(set_, &hi, &haptic_))) {
            haptic_ = XR_NULL_HANDLE;
            LOGW("vr hotkey: xrCreateAction(haptic) failed -- no vibration feedback");
        }
        prepareFailed_ = false;
    }
    // ---- suggestions: every profile the game suggested (merged), or all of ours when it suggested none
    std::vector<std::string> profiles;
    std::map<std::string, std::vector<XrActionSuggestedBinding>> base;   // the game's bindings per profile: its own or translated
    std::map<std::string, bool> translated;
    const bool appHas = !appSuggest_.empty();
    if (appHas) {
        const char* const kSimple = "/interaction_profiles/khr/simple_controller";
        const std::vector<XrActionSuggestedBinding>* simple = nullptr;
        bool onlySimple = true;
        for (const auto& kv : appSuggest_) {
            const std::string p = PathString(kv.first);
            if (p == kSimple) simple = &kv.second; else onlySimple = false;
            bool known = false;
            for (const char* k : VrKnownProfiles()) known = known || p == k;
            if (known) { profiles.push_back(p); base[p] = kv.second; }
            else LOGI("vr hotkey: profile %s (suggested by the game) is not one the layer knows -- no controller hotkey on it",
                      p.c_str());
        }
        if (onlySimple && simple) {
            // ATS / ETS2: only the simple controller (no thumbsticks). The real profiles get the game's bindings translated, so
            // the runtime picks the real controller and the game keeps its buttons (select = trigger, menu = menu).
            std::string names;
            size_t dropped = 0;
            for (const char* t : VrSimpleTranslationTargets()) {
                std::vector<XrActionSuggestedBinding> tr;
                for (const XrActionSuggestedBinding& sb : *simple) {
                    std::string op;
                    if (VrTranslateSimpleBinding(t, PathString(sb.binding), op)) tr.push_back({sb.action, Path(op)});
                    else ++dropped;
                }
                profiles.push_back(t);
                base[t] = tr;
                translated[t] = true;
                names += (names.empty() ? "" : ", ") + ShortProfile(t);
            }
            LOGI("vr hotkey: the game suggests only khr/simple_controller (%zu bindings, no thumbsticks) -- also suggesting %s "
                 "with the game's bindings translated (select -> trigger, menu -> menu; %zu without an equivalent left out), so "
                 "the runtime can use the real controller and the game keeps its buttons", simple->size(), names.c_str(), dropped);
        }
    } else {
        for (const char* k : VrKnownProfiles()) profiles.push_back(k);
    }
    int ok = 0;
    std::string summary;
    for (const std::string& prof : profiles) {
        std::vector<XrActionSuggestedBinding> ours;
        std::string paths, missing;
        for (size_t i = 0; i < hk_.buttons.size(); ++i) {
            std::string p;
            if (!VrButtonPath(prof, hk_.buttons[i], p)) { missing = VrButtonName(hk_.buttons[i]); break; }
            ours.push_back({buttons_[i], Path(p)});
            paths += (paths.empty() ? "" : " + ") + p;
        }
        if (!missing.empty()) {
            if (appHas) LOGI("vr hotkey: profile %s has no %s -- the combination is not available with it", ShortProfile(prof).c_str(), missing.c_str());
            continue;
        }
        if (haptic_) {
            ours.push_back({haptic_, Path("/user/hand/left/output/haptic")});
            ours.push_back({haptic_, Path("/user/hand/right/output/haptic")});
        }
        const XrPath pp = Path(prof);
        auto it = appSuggest_.find(pp);
        std::vector<XrActionSuggestedBinding> all;
        auto bit = base.find(prof);
        if (bit != base.end()) all = bit->second;
        const size_t nApp = all.size();
        const bool isTr = translated.count(prof) != 0;
        all.insert(all.end(), ours.begin(), ours.end());
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = pp;
        sb.countSuggestedBindings = (uint32_t)all.size();
        sb.suggestedBindings = all.data();
        const XrResult r = d_.SuggestInteractionProfileBindings(inst_, &sb);
        if (XR_SUCCEEDED(r)) {
            ++ok;
            LOGI("vr hotkey: binding %s on %s (%s)", paths.c_str(), ShortProfile(prof).c_str(),
                 it != appSuggest_.end() ? ("merged into the game's " + std::to_string(nApp) + " bindings of this profile").c_str()
                 : isTr ? ("with the game's " + std::to_string(nApp) + " simple_controller bindings translated").c_str()
                        : "the game suggests no profile: ours only");
            summary += (summary.empty() ? "" : ", ") + ShortProfile(prof);
            continue;
        }
        if (it != appSuggest_.end()) {
            // restore the game's own suggestion exactly (a refused call should change nothing; this makes sure)
            XrInteractionProfileSuggestedBinding back{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            back.interactionProfile = pp;
            back.countSuggestedBindings = (uint32_t)it->second.size();
            back.suggestedBindings = it->second.data();
            const XrResult rb = d_.SuggestInteractionProfileBindings(inst_, &back);
            LOGW("vr hotkey: the runtime refused our bindings on %s (%d) -- the game's own %zu bindings re-suggested (%d)",
                 ShortProfile(prof).c_str(), (int)r, it->second.size(), (int)rb);
        } else {
            if (isTr) LOGI("vr hotkey: profile %s not accepted by the runtime (%d) -- skipped", ShortProfile(prof).c_str(), (int)r);
            else LOGD("vr hotkey: profile %s not accepted by the runtime (%d) -- skipped", ShortProfile(prof).c_str(), (int)r);
        }
    }
    if (!ok) {
        LOGW("vr hotkey: no controller profile takes the combination %s -- controller hotkey off for this game",
             elog::Narrow(hk_.text).c_str());
        prepareFailed_ = true;
        return false;
    }
    ownSummary_ = summary;
    prepared_ = true;
    return true;
}

XrResult VrInput::Attach(XrSession s, const XrSessionActionSetsAttachInfo* ai) {
    std::unique_lock<std::mutex> lk(m_);
    auto it = sessions_.find(s);
    if (it == sessions_.end() || !ai) { lk.unlock(); return d_.AttachSessionActionSets(s, ai); }
    SessionState& ss = it->second;
    ss.appAttach = true;
    if (!enabled_ || !ss.armed || ss.off) { lk.unlock(); return d_.AttachSessionActionSets(s, ai); }
    if (ss.ours) {   // we attached ours to a game "without input": its own attach cannot work any more
        LOGE("vr hotkey: the game attaches its action sets after the layer attached its own (it had none for %.1f s) -- the "
             "runtime will refuse the game's call; set vr_hotkey = off for this game", selfAttachS_);
        lk.unlock();
        return d_.AttachSessionActionSets(s, ai);
    }
    std::vector<XrActionSet> list(ai->actionSets, ai->actionSets + (ai->actionSets ? ai->countActionSets : 0));
    const bool ready = frozen_ ? prepared_ : PrepareLocked(list);
    if (!ready) {
        const XrResult r = d_.AttachSessionActionSets(s, ai);
        if (XR_SUCCEEDED(r)) frozen_ = true;
        return r;
    }
    list.push_back(set_);
    XrSessionActionSetsAttachInfo a2 = *ai;
    a2.countActionSets = (uint32_t)list.size();
    a2.actionSets = list.data();
    const XrResult r = d_.AttachSessionActionSets(s, &a2);
    if (XR_SUCCEEDED(r)) {
        frozen_ = true;
        ss.ours = true;
        LOGI("vr hotkey: %s held %.1f s = start / stop; attached with the game's %u action set(s) (profiles: %s)",
             elog::Narrow(hk_.text).c_str(), holdS_, ai->countActionSets, ownSummary_.c_str());
        return r;
    }
    const XrResult r2 = d_.AttachSessionActionSets(s, ai);
    if (XR_SUCCEEDED(r2)) {
        frozen_ = true;
        ss.off = true;
        LOGW("vr hotkey: xrAttachSessionActionSets with our action set failed (%d); the game's own sets attached alone -- "
             "controller hotkey off for this session", (int)r);
    }
    return r2;
}

XrResult VrInput::Sync(XrSession s, const XrActionsSyncInfo* si) {
    std::unique_lock<std::mutex> lk(m_);
    auto it = sessions_.find(s);
    if (it == sessions_.end() || !si || !it->second.ours || it->second.off || it->second.self) {
        lk.unlock();
        return d_.SyncActions(s, si);
    }
    std::vector<XrActiveActionSet> act(si->activeActionSets, si->activeActionSets + (si->activeActionSets ? si->countActiveActionSets : 0));
    act.push_back({set_, XR_NULL_PATH});
    lk.unlock();
    XrActionsSyncInfo s2 = *si;
    s2.countActiveActionSets = (uint32_t)act.size();
    s2.activeActionSets = act.data();
    const XrResult r = d_.SyncActions(s, &s2);
    if (XR_FAILED(r)) {
        const XrResult r2 = d_.SyncActions(s, si);
        if (XR_SUCCEEDED(r2)) {
            lk.lock();
            auto j = sessions_.find(s);
            if (j != sessions_.end()) {
                j->second.off = true;
                LOGW("vr hotkey: xrSyncActions with our action set failed (%d), the game's own call succeeds -- controller hotkey "
                     "off for this session", (int)r);
            }
        }
        return r2;
    }
    Evaluate(s, r == XR_SUCCESS);   // XR_SESSION_NOT_FOCUSED: the inputs are inactive
    return r;
}

void VrInput::EndFrame(XrSession s) {
    if (!enabled_) return;
    std::unique_lock<std::mutex> lk(m_);
    auto it = sessions_.find(s);
    if (it == sessions_.end() || !it->second.armed) return;
    SessionState& ss = it->second;
    const double now = VrNowS();
    if (ss.firstFrame < 0) ss.firstFrame = now;
    if (!ss.ours && !ss.appAttach && !ss.off && !frozen_ && appSets_.empty() && appSuggest_.empty() &&
        now - ss.firstFrame >= selfAttachS_) {
        if (!PrepareLocked({})) {
            ss.off = true;
            return;
        }
        XrSessionActionSetsAttachInfo a{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        a.countActionSets = 1;
        a.actionSets = &set_;
        const XrResult r = d_.AttachSessionActionSets(s, &a);
        if (XR_FAILED(r)) {
            ss.off = true;
            LOGW("vr hotkey: attaching our own action set (the game has none) failed (%d) -- controller hotkey off", (int)r);
            return;
        }
        frozen_ = true;
        ss.ours = ss.self = true;
        LOGI("vr hotkey: the game has no controller input of its own (no action set after %.1f s of frames) -- the layer attached "
             "its own action set and syncs it every frame; %s held %.1f s = start / stop (profiles: %s)", selfAttachS_,
             elog::Narrow(hk_.text).c_str(), holdS_, ownSummary_.c_str());
    }
    if (!ss.self || !ss.ours || ss.off) return;
    XrActiveActionSet as{set_, XR_NULL_PATH};
    lk.unlock();
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &as;
    const XrResult r = d_.SyncActions(s, &si);
    if (XR_FAILED(r)) {
        lk.lock();
        auto j = sessions_.find(s);
        if (j != sessions_.end() && !j->second.off) {
            j->second.off = true;
            LOGW("vr hotkey: xrSyncActions of our own action set failed (%d) -- controller hotkey off for this session", (int)r);
        }
        return;
    }
    Evaluate(s, r == XR_SUCCESS);
}

void VrInput::Evaluate(XrSession s, bool focused) {
    bool all = focused;
    for (size_t i = 0; all && i < buttons_.size(); ++i) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = buttons_[i];
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        const XrResult r = d_.GetActionStateBoolean(s, &gi, &st);
        all = XR_SUCCEEDED(r) && st.isActive && st.currentState;
    }
    const double now = VrNowS();
    bool fire = false, checkProfile = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = sessions_.find(s);
        if (it == sessions_.end()) return;
        SessionState& ss = it->second;
        ++ss.syncs;
        if (!ss.loggedFirstSync) {
            ss.loggedFirstSync = true;
            LOGI("vr hotkey: first input sync with our action set (%s)", ss.self ? "synced by the layer" : "appended to the game's xrSyncActions");
        }
        fire = ss.hold.Update(all, now, holdS_);
        if (ss.profileLogs < 12 && (ss.lastProfileCheck < 0 || now - ss.lastProfileCheck >= 2.0)) {
            ss.lastProfileCheck = now;
            checkProfile = true;
        }
    }
    if (checkProfile) {   // which controller profile the runtime picked (logged when it changes)
        std::string cur[2];
        const char* const tops[2] = {"/user/hand/left", "/user/hand/right"};
        for (int h = 0; h < 2; ++h) {
            XrPath top = XR_NULL_PATH;
            {
                std::lock_guard<std::mutex> lk(m_);
                top = Path(tops[h]);
            }
            XrInteractionProfileState ip{XR_TYPE_INTERACTION_PROFILE_STATE};
            if (top != XR_NULL_PATH && XR_SUCCEEDED(d_.GetCurrentInteractionProfile(s, top, &ip))) {
                std::lock_guard<std::mutex> lk(m_);
                cur[h] = PathString(ip.interactionProfile);
            }
        }
        std::lock_guard<std::mutex> lk(m_);
        auto it = sessions_.find(s);
        if (it != sessions_.end() && (cur[0] != it->second.profile[0] || cur[1] != it->second.profile[1])) {
            it->second.profile[0] = cur[0];
            it->second.profile[1] = cur[1];
            ++it->second.profileLogs;
            LOGI("vr hotkey: the runtime uses %s (left hand) / %s (right hand)", cur[0].empty() ? "no profile" : ShortProfile(cur[0]).c_str(),
                 cur[1].empty() ? "no profile" : ShortProfile(cur[1]).c_str());
        }
    }
    if (!fire) return;
    const int k = activate_ ? activate_(s) : 0;
    LOGI("vr hotkey: %s held %.1f s -> %s", elog::Narrow(hk_.text).c_str(), holdS_,
         k == 1 ? "start recording" : k == 2 ? "stop recording" : "ignored (no capture-capable session, or the last recording is still finalizing)");
    if (k) Haptic(s, k);
}

void VrInput::Haptic(XrSession s, int kind) {
    if (!haptic_) return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = 0.6f;
    v.duration = kind == 1 ? 100000000 : 300000000;   // start: 0.1 s, stop: 0.3 s
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = haptic_;
    const XrResult r = d_.ApplyHapticFeedback(s, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
    LOGI("vr hotkey: haptic pulse %.1f s on both controllers (%d)", kind == 1 ? 0.1 : 0.3, (int)r);
}
