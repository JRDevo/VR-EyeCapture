// control -- see control.h.
#include "control.h"
#include <windows.h>
#include <atomic>
#include "log.h"
#include "shared_status.h"

namespace {
std::atomic<bool> s_stop{false};
HANDLE            s_thread = nullptr;
Config            s_cfg;
std::wstring      s_logPath, s_configPath;
ControlHooks      s_hooks;

void Cue(int kind) {
    switch (kind) {
    case 0: Beep(880, 160); break;
    case 1: Beep(660, 90); Sleep(70); Beep(660, 90); break;
    default: Beep(220, 450); break;
    }
}

bool HotkeyDown(const Hotkey& h) {
    if (!h.vk) return false;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (!down((int)h.vk)) return false;
    return down(VK_CONTROL) == h.ctrl && down(VK_SHIFT) == h.shift && down(VK_MENU) == h.alt;
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

DWORD WINAPI ThreadProc(LPVOID param) {
    HMODULE self = (HMODULE)param;   // pinned by ControlStart; released by FreeLibraryAndExitThread
    const DWORD pid = GetCurrentProcessId();
    HANDLE mapping = nullptr;
    EyeCapShared* shm = EyeCapOpenShared(&mapping);
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, EYECAP_EVENT_NAME);
    if (!shm) LOGW("control: shared memory unavailable -- tray app control disabled (hotkey still works)");
    LONG lastSeq = shm ? shm->cmdSeq : 0;
    bool prevDown = false;
    bool pendingStart = false;
    ULONGLONG pendingSince = 0, lastPublish = 0;
    RecState prevState = RecState::Idle;
    bool owner = false;

    while (!s_stop.load()) {
        if (evt) WaitForSingleObject(evt, 20); else Sleep(20);
        if (s_stop.load()) break;
        CaptureSnapshot snap = s_hooks.snapshot();
        const ULONGLONG now = GetTickCount64();

        // ownership of the shared status block (the newest capture-capable game wins a dead / stale owner)
        if (shm) {
            if (snap.capable) {
                const DWORD cur = shm->ownerPid;
                if (cur != pid && (cur == 0 || !EyeCapProcessAlive(cur) || now - shm->heartbeatTick > 3000)) {
                    shm->ownerPid = pid;
                    LOGI("control: this process now owns the tray status (pid %lu)", (unsigned long)pid);
                }
                owner = shm->ownerPid == pid;
            } else if (owner || shm->ownerPid == pid) {
                EyeCapBeginWrite(shm);
                shm->ownerPid = 0;
                shm->state = ECS_NONE;
                EyeCapEndWrite(shm);
                owner = false;
            }
        }

        // requests: hotkey (owner only, or any capable process without shared memory) + tray commands
        int request = 0;   // 1 toggle, 2 start, 3 stop
        const bool down = HotkeyDown(s_cfg.hotkey);
        if (down && !prevDown && snap.capable && (owner || !shm)) {
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

        // publish for the tray
        if (shm && owner && now - lastPublish >= 250) {
            lastPublish = now;
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
    }
    if (shm) {
        if (shm->ownerPid == pid) {
            EyeCapBeginWrite(shm);
            shm->ownerPid = 0;
            shm->state = ECS_NONE;
            EyeCapEndWrite(shm);
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
    s_cfg = cfg;
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
    LOGI("control: hotkey %s (exact modifiers), tray IPC %s", elog::Narrow(cfg.hotkey.text).c_str(),
         elog::Narrow(EYECAP_SHM_NAME).c_str());
}

void ControlStop() {
    if (!s_thread) return;
    s_stop = true;
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, EYECAP_EVENT_NAME);
    if (e) { SetEvent(e); CloseHandle(e); }
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = nullptr;
}
