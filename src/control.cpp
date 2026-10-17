// control -- see control.h.
#include "control.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include "log.h"
#include "shared_status.h"

namespace {
std::atomic<bool> s_stop{false};
HANDLE            s_thread = nullptr;
Config            s_cfg;
std::wstring      s_logPath, s_configPath;
// Test-only: XR_EYE_CAPTURE_TEST_SHM=<name> puts the status block on a private name (event = <name>-Command) so
// tests/control_final_test.exe never touches a real game's / the real tray's block.
std::wstring      s_shmName = EYECAP_SHM_NAME, s_eventName = EYECAP_EVENT_NAME;
ControlHooks      s_hooks;
// Guards the shared status block between the control thread and ControlPublishFinal (called from the game's
// xrDestroySession thread). Lock order: s_pubLock -> g_lock (snapshot()); nobody calls into control holding g_lock.
std::mutex        s_pubLock;
std::atomic<bool> s_beeps{true};         // config beeps (ControlStart, ControlSetBeeps at every config reload)
EyeCapShared*     s_shm = nullptr;       // valid while the control thread runs (set / cleared under s_pubLock)
bool              s_owner = false;
ULONGLONG         s_lastPublish = 0;

void Cue(int kind) {
    if (!s_beeps.load()) return;   // beeps = 0: the cues play on the default device, which is often the recorded loopback one
    switch (kind) {
    case 0: Beep(880, 160); break;
    case 1: Beep(660, 90); Sleep(70); Beep(660, 90); break;
    default: Beep(220, 450); break;
    }
}

// True while ANY keyboard / mouse binding of the list is held with exactly its modifiers. Pad: / Joy: bindings are the
// tray's job (tray/input.cpp sends ECC_TOGGLE); the layer ignores them.
bool HotkeyDown(const Hotkey& h) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool mods = false, ctrl = false, shift = false, alt = false;
    for (const HotkeyBinding& b : h.bindings) {
        if (b.kind != HotkeyBinding::Kind::Key || !b.vk || !down((int)b.vk)) continue;
        if (!mods) { ctrl = down(VK_CONTROL); shift = down(VK_SHIFT); alt = down(VK_MENU); mods = true; }
        if (ctrl == b.ctrl && shift == b.shift && alt == b.alt) return true;
    }
    return false;
}

uint32_t MapState(const CaptureSnapshot& s) {
    if (!s.capable) return ECS_NONE;
    switch (s.rec.state) {
    case RecState::Starting:  return ECS_STARTING;
    case RecState::Recording: return ECS_RECORDING;
    case RecState::Stopping:  return ECS_STOPPING;
    case RecState::Failed:    return ECS_FAILED;
    default:                  return ECS_READY;
    }
}

// Writes the whole status block from a snapshot. Caller holds s_pubLock and owns the block.
void PublishLocked(EyeCapShared* shm, const CaptureSnapshot& snap, ULONGLONG now) {
    const uint32_t newState = MapState(snap);
    EyeCapBeginWrite(shm);
    if (newState == ECS_RECORDING && shm->state != ECS_RECORDING) shm->recStartTick = now;
    if (newState != ECS_RECORDING && newState != ECS_STOPPING) shm->recStartTick = 0;
    shm->state = newState;
    shm->width = snap.rec.width;
    shm->height = snap.rec.height;
    shm->fps = snap.rec.fps;
    shm->framesWritten = snap.rec.framesWritten;
    shm->framesDropped = snap.rec.framesDropped;
    shm->heartbeatTick = now;
    EyeCapCopy(shm->game, 64, snap.game.c_str());
    EyeCapCopy(shm->api, 16, snap.api.c_str());
    EyeCapCopy(shm->runtime, 64, snap.runtime.c_str());
    EyeCapCopy(shm->hotkey, 32, s_cfg.hotkey.text.c_str());
    EyeCapCopy(shm->file, 260, snap.rec.file.c_str());
    EyeCapCopy(shm->outputDir, 260, snap.outputDir.c_str());
    EyeCapCopy(shm->logPath, 260, s_logPath.c_str());
    EyeCapCopy(shm->configPath, 260, s_configPath.c_str());
    EyeCapCopy(shm->message, 160, elog::Widen(snap.rec.message).c_str());
    EyeCapEndWrite(shm);
}

DWORD WINAPI ThreadProc(LPVOID param) {
    HMODULE self = (HMODULE)param;   // pinned by ControlStart; released by FreeLibraryAndExitThread
    const DWORD pid = GetCurrentProcessId();
    HANDLE mapping = nullptr;
    EyeCapShared* shm = EyeCapOpenShared(&mapping, s_shmName.c_str());
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, s_eventName.c_str());
    if (!shm) LOGW("control: shared memory unavailable -- tray app control disabled (hotkey still works)");
    {
        std::lock_guard<std::mutex> pl(s_pubLock);
        s_shm = shm;
        s_owner = false;
        s_lastPublish = 0;
    }
    LONG lastSeq = shm ? shm->cmdSeq : 0;
    bool prevDown = false;
    bool pendingStart = false;
    ULONGLONG pendingSince = 0;
    RecState prevState = RecState::Idle;
    RecState prevPubState = RecState::Idle;

    while (!s_stop.load()) {
        if (evt) WaitForSingleObject(evt, 20); else Sleep(20);
        if (s_stop.load()) break;
        CaptureSnapshot snap = s_hooks.snapshot();
        const ULONGLONG now = GetTickCount64();

        // ownership of the shared status block (the newest capture-capable game wins a dead / stale owner)
        if (shm) {
            std::lock_guard<std::mutex> pl(s_pubLock);
            if (snap.capable) {
                const DWORD cur = shm->ownerPid;
                if (cur != pid && (cur == 0 || !EyeCapProcessAlive(cur) || now - shm->heartbeatTick > 3000)) {
                    shm->ownerPid = pid;
                    LOGI("control: this process now owns the tray status (pid %lu)", (unsigned long)pid);
                }
                s_owner = shm->ownerPid == pid;
            } else if (s_owner || shm->ownerPid == pid) {
                // only ownerPid / state change: file + message ("saved <file>") stay so the tray can still read them
                EyeCapBeginWrite(shm);
                shm->ownerPid = 0;
                shm->state = ECS_NONE;
                EyeCapEndWrite(shm);
                s_owner = false;
            }
        }
        // requests: hotkey (owner only, or any capable process without shared memory) + tray commands
        int request = 0;   // 1 toggle, 2 start, 3 stop
        const bool down = HotkeyDown(s_cfg.hotkey);
        if (down && !prevDown && snap.capable && (s_owner || !shm)) {
            request = 1;
            LOGI("control: hotkey %s pressed", elog::Narrow(s_cfg.hotkey.text).c_str());
        }
        prevDown = down;
        if (shm && shm->cmdSeq != lastSeq) {
            lastSeq = shm->cmdSeq;
            MemoryBarrier();
            if (shm->cmdTargetPid == pid) {
                request = shm->cmd == ECC_START ? 2 : shm->cmd == ECC_STOP ? 3 : shm->cmd == ECC_TOGGLE ? 1 : 0;
                LOGI("control: tray command %u", (unsigned)shm->cmd);
            }
        }
        if (request) {
            const RecState st = snap.rec.state;
            const bool active = st == RecState::Starting || st == RecState::Recording;
            const bool wantStart = request == 2 || (request == 1 && !active);
            const bool wantStop = request == 3 || (request == 1 && active);
            if (!snap.capable) {
                LOGW("control: no capture-capable OpenXR session in this process");
                Cue(2);
            } else if (wantStop && active) {
                s_hooks.requestStop();
                pendingStart = false;
                LOGI("control: stop requested");
                Cue(1);
            } else if (wantStart && !active) {
                if (st == RecState::Stopping) {
                    LOGW("control: the previous recording is still finalizing -- try again in a moment");
                    Cue(2);
                } else {
                    s_hooks.requestStart();
                    pendingStart = true;
                    pendingSince = now;
                    LOGI("control: start requested (begins at the next submitted frame)");
                }
            }
        }
        if (pendingStart) {
            if (snap.rec.state == RecState::Recording) {
                pendingStart = false;
                Cue(0);
            } else if (snap.rec.state == RecState::Failed && now - pendingSince > 100) {
                pendingStart = false;
                LOGE("control: recording failed to start: %s", snap.rec.message.c_str());
                Cue(2);
            } else if (now - pendingSince > 6000 && snap.rec.state != RecState::Starting) {
                pendingStart = false;
                s_hooks.requestStop();
                LOGE("control: no frame with a projection layer arrived within 6 s -- start cancelled");
                Cue(2);
            }
        }
        if (prevState != snap.rec.state) {
            LOGD("control: recorder state %s -> %s", RecStateName(prevState), RecStateName(snap.rec.state));
            prevState = snap.rec.state;
        }

        // publish for the tray: every 250 ms, and at once when a finalize just completed ("saved <file>")
        if (prevPubState == RecState::Stopping && snap.rec.state != RecState::Stopping) s_lastPublish = 0;
        prevPubState = snap.rec.state;
        if (shm) {
            std::lock_guard<std::mutex> pl(s_pubLock);
            if (s_owner && now - s_lastPublish >= 250) {
                s_lastPublish = now;
                // re-take the snapshot under the lock so a stale one never overwrites a ControlPublishFinal
                const CaptureSnapshot cur = s_hooks.snapshot();
                if (cur.capable) PublishLocked(shm, cur, now);
            }
        }
    }
    if (shm) {
        {
            std::lock_guard<std::mutex> pl(s_pubLock);
            if (shm->ownerPid == pid) {
                EyeCapBeginWrite(shm);
                shm->ownerPid = 0;   // message / file stay intact ("saved <file>" is read after the game is gone)
                shm->state = ECS_NONE;
                EyeCapEndWrite(shm);
            }
            s_shm = nullptr;
            s_owner = false;
        }
        UnmapViewOfFile(shm);
    }
    if (mapping) CloseHandle(mapping);
    if (evt) CloseHandle(evt);
    if (self) FreeLibraryAndExitThread(self, 0);
    return 0;
}
} // namespace

void ControlStart(const Config& cfg, const std::wstring& logPath, const std::wstring& configPath, ControlHooks hooks) {
    if (s_thread) return;
    wchar_t testShm[128];
    if (GetEnvironmentVariableW(L"XR_EYE_CAPTURE_TEST_SHM", testShm, 128) > 0) {
        s_shmName = testShm;
        s_eventName = std::wstring(testShm) + L"-Command";
    }
    s_cfg = cfg;
    s_beeps = cfg.beeps;
    s_logPath = logPath;
    s_configPath = configPath;
    s_hooks = std::move(hooks);
    s_stop = false;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&ThreadProc, &self)) self = nullptr;
    s_thread = CreateThread(nullptr, 0, ThreadProc, self, 0, nullptr);
    if (!s_thread) {
        if (self) FreeLibrary(self);
        LOGE("control: CreateThread failed -- hotkey and tray control disabled");
        return;
    }
    LOGI("control: hotkey %s (keyboard / mouse: exact modifiers; Pad: / Joy: are read by the tray), tray IPC %s, beeps %s",
         elog::Narrow(cfg.hotkey.text).c_str(), elog::Narrow(s_shmName).c_str(), cfg.beeps ? "on" : "off");
}

void ControlSetBeeps(bool on) {
    if (s_beeps.exchange(on) != on) LOGI("control: beeps %s (config reload)", on ? "on" : "off");
}

void ControlStop() {
    if (!s_thread) return;
    s_stop = true;
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, s_eventName.c_str());
    if (e) { SetEvent(e); CloseHandle(e); }
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = nullptr;
}

void ControlPublishFinal(const RecorderStatus& rec, const std::wstring& outputDir) {
    std::lock_guard<std::mutex> pl(s_pubLock);
    EyeCapShared* shm = s_shm;
    if (!shm) return;
    const DWORD pid = GetCurrentProcessId();
    const DWORD cur = shm->ownerPid;
    // never write over another live game's status; a block we released (0) or a dead owner's is fine
    if (cur != 0 && cur != pid && EyeCapProcessAlive(cur)) return;
    const ULONGLONG now = GetTickCount64();
    EyeCapBeginWrite(shm);
    if (cur == pid) {                       // still the owner: live fields too; ownerPid / state are left to the loop
        shm->state = ECS_READY;
        shm->recStartTick = 0;
        shm->heartbeatTick = now;
    }
    shm->width = rec.width;
    shm->height = rec.height;
    shm->fps = rec.fps;
    shm->framesWritten = rec.framesWritten;
    shm->framesDropped = rec.framesDropped;
    EyeCapCopy(shm->file, 260, rec.file.c_str());
    if (!outputDir.empty()) EyeCapCopy(shm->outputDir, 260, outputDir.c_str());
    EyeCapCopy(shm->message, 160, elog::Widen(rec.message).c_str());
    EyeCapEndWrite(shm);
    LOGI("control: published final status to the tray: %s", rec.message.c_str());
}
