// audio_capture -- see audio_capture.h.
#include "audio_capture.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include "frame_source.h"   // QpcNow100ns
#include "log.h"

using Microsoft::WRL::ComPtr;

namespace {
std::wstring LowerW(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

std::wstring FriendlyName(IMMDevice* d) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return L"?";
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring name = L"?";
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

enum class SampleKind { F32, I16, I24, I32, Unknown };

struct Resampler {
    double step = 1.0;           // input frames per output frame
    double frac = 0.0;
    bool havePrev = false;
    float prev[2] = {0, 0};
    // in: stereo float frames (n). Appends 48 kHz stereo float frames to out.
    void Process(const float* in, uint32_t n, std::vector<float>& out) {
        if (n == 0) return;
        if (step == 1.0) { out.insert(out.end(), in, in + 2 * n); return; }
        if (!havePrev) { prev[0] = in[0]; prev[1] = in[1]; havePrev = true; }
        auto at = [&](uint32_t i, int ch) -> float { return i == 0 ? prev[ch] : in[(size_t)(i - 1) * 2 + ch]; };
        while (frac < (double)n) {
            const uint32_t i = (uint32_t)frac;
            const float t = (float)(frac - i);
            out.push_back(at(i, 0) * (1 - t) + at(i + 1, 0) * t);
            out.push_back(at(i, 1) * (1 - t) + at(i + 1, 1) * t);
            frac += step;
        }
        frac -= n;
        prev[0] = in[(size_t)(n - 1) * 2];
        prev[1] = in[(size_t)(n - 1) * 2 + 1];
    }
};
} // namespace

AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Start(const std::wstring& deviceSubstring, Sink sink) {
    Stop();
    sink_ = std::move(sink);
    deviceSubstring_ = deviceSubstring;
    stop_ = false;
    initState_ = 0;
    written_ = 0;
    framesOut_ = 0; silenceFrames_ = 0; droppedFrames_ = 0;
    thread_ = std::thread([this] { Run(); });
    const ULONGLONG t0 = GetTickCount64();
    while (initState_.load() == 0 && GetTickCount64() - t0 < 5000) Sleep(2);
    if (initState_.load() != 1) {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void AudioCapture::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::Emit(const int16_t* pcm, uint32_t frames) {
    if (!frames) return;
    const int64_t start = t0_ + (int64_t)(written_ * 10000000ull / kRate);
    sink_(pcm, frames, start);
    written_ += frames;
    framesOut_ += frames;
}

void AudioCapture::EmitSilence(uint64_t frames) {
    static const int16_t zeros[480 * 2] = {};
    silenceFrames_ += frames;
    while (frames) {
        const uint32_t n = (uint32_t)std::min<uint64_t>(frames, 480);
        Emit(zeros, n);
        frames -= n;
    }
}

void AudioCapture::Run() {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;
    WAVEFORMATEX* wfx = nullptr;
    SampleKind kind = SampleKind::Unknown;
    UINT32 channels = 0, rate = 0, blockAlign = 0;
    float gain[32][2] = {};
    Resampler rs;
    std::vector<float> stereo, resampled;
    std::vector<int16_t> pcm;
    int64_t lastPacketQpc = 0;

    auto fail = [&](const char* what, HRESULT hr) {
        LOGE("audio: %s failed hr=0x%08lx -- recording continues without audio", what, (unsigned long)hr);
        initState_ = -1;
    };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) { fail("MMDeviceEnumerator", hr); goto done; }
    if (!deviceSubstring_.empty()) {
        ComPtr<IMMDeviceCollection> coll;
        UINT count = 0;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && SUCCEEDED(coll->GetCount(&count))) {
            const std::wstring want = LowerW(deviceSubstring_);
            for (UINT i = 0; i < count && !dev; ++i) {
                ComPtr<IMMDevice> d;
                if (SUCCEEDED(coll->Item(i, &d)) && LowerW(FriendlyName(d.Get())).find(want) != std::wstring::npos) dev = d;
            }
        }
        if (!dev) LOGW("audio: no active render device matches '%s' -- using the default render device",
                       elog::Narrow(deviceSubstring_).c_str());
    }
    if (!dev) {
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) { fail("GetDefaultAudioEndpoint", hr); goto done; }
    }
    deviceName_ = elog::Narrow(FriendlyName(dev.Get()));
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
    if (FAILED(hr)) { fail("IAudioClient activate", hr); goto done; }
    hr = client->GetMixFormat(&wfx);
    if (FAILED(hr)) { fail("GetMixFormat", hr); goto done; }
    {
        channels = wfx->nChannels;
        rate = wfx->nSamplesPerSec;
        blockAlign = wfx->nBlockAlign;
        DWORD mask = 0;
        WORD validBits = wfx->wBitsPerSample;
        GUID sub = GUID_NULL;
        if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wfx);
            sub = ext->SubFormat;
            mask = ext->dwChannelMask;
            validBits = ext->Samples.wValidBitsPerSample;
        } else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            sub = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
            sub = KSDATAFORMAT_SUBTYPE_PCM;
        }
        if (sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && wfx->wBitsPerSample == 32) kind = SampleKind::F32;
        else if (sub == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 16) kind = SampleKind::I16;
        else if (sub == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 24) kind = SampleKind::I24;
        else if (sub == KSDATAFORMAT_SUBTYPE_PCM && wfx->wBitsPerSample == 32) kind = SampleKind::I32;
        char ft[160];
        snprintf(ft, sizeof(ft), "%u Hz, %u ch (mask 0x%lx), %u bit (%u valid) %s", rate, channels, (unsigned long)mask,
                 wfx->wBitsPerSample, validBits,
                 kind == SampleKind::F32 ? "float" : kind == SampleKind::Unknown ? "UNSUPPORTED" : "int");
        formatText_ = ft;
        if (kind == SampleKind::Unknown || channels == 0 || channels > 32 || rate < 8000) {
            LOGE("audio: unsupported mix format (%s) -- recording continues without audio", ft);
            initState_ = -1;
            goto done;
        }
        // Downmix gains to stereo. Channels appear in the order of the set mask bits.
        if (channels == 1) { gain[0][0] = gain[0][1] = 1.0f; }
        else if (!mask || channels == 2) { gain[0][0] = 1; gain[1][1] = 1; }
        else {
            UINT ch = 0;
            for (DWORD bit = 1; bit && ch < channels; bit <<= 1) {
                if (!(mask & bit)) continue;
                float l = 0, r = 0;
                switch (bit) {
                case SPEAKER_FRONT_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: l = 1; break;
                case SPEAKER_FRONT_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: r = 1; break;
                case SPEAKER_FRONT_CENTER: l = r = 0.7071f; break;
                case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: l = 0.7071f; break;
                case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: r = 0.7071f; break;
                case SPEAKER_BACK_CENTER: l = r = 0.5f; break;
                default: break;   // LFE and top channels dropped
                }
                gain[ch][0] = l; gain[ch][1] = r;
                ++ch;
            }
        }
        rs.step = (double)rate / (double)kRate;
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000 /*200 ms*/, 0, wfx, nullptr);
    if (FAILED(hr)) { fail("IAudioClient::Initialize(loopback)", hr); goto done; }
    hr = client->GetService(IID_PPV_ARGS(&cap));
    if (FAILED(hr)) { fail("GetService(IAudioCaptureClient)", hr); goto done; }
    t0_ = QpcNow100ns();
    hr = client->Start();
    if (FAILED(hr)) { fail("IAudioClient::Start", hr); goto done; }
    LOGI("audio: WASAPI loopback on '%s' (%s) -> AAC 48 kHz stereo", deviceName_.c_str(), formatText_.c_str());
    initState_ = 1;
    lastPacketQpc = t0_;

    while (!stop_.load()) {
        Sleep(5);
        UINT32 next = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&next)) && next > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) break;
            stereo.assign((size_t)frames * 2, 0.0f);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                for (UINT32 i = 0; i < frames; ++i) {
                    const BYTE* fr = data + (size_t)i * blockAlign;
                    float l = 0, r = 0;
                    for (UINT32 c = 0; c < channels; ++c) {
                        float s = 0;
                        switch (kind) {
                        case SampleKind::F32: s = reinterpret_cast<const float*>(fr)[c]; break;
                        case SampleKind::I16: s = reinterpret_cast<const int16_t*>(fr)[c] / 32768.0f; break;
                        case SampleKind::I24: {
                            const BYTE* p = fr + c * 3;
                            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
                            s = v / 8388608.0f;
                            break;
                        }
                        case SampleKind::I32: s = reinterpret_cast<const int32_t*>(fr)[c] / 2147483648.0f; break;
                        default: break;
                        }
                        l += s * gain[c][0];
                        r += s * gain[c][1];
                    }
                    stereo[(size_t)i * 2] = l;
                    stereo[(size_t)i * 2 + 1] = r;
                }
            }
            cap->ReleaseBuffer(frames);
            lastPacketQpc = QpcNow100ns();

            resampled.clear();
            rs.Process(stereo.data(), frames, resampled);
            uint32_t outFrames = (uint32_t)(resampled.size() / 2);
            pcm.resize(resampled.size());
            for (size_t i = 0; i < resampled.size(); ++i) {
                float v = resampled[i];
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                pcm[i] = (int16_t)lrintf(v * 32767.0f);
            }

            // Align to the QPC timeline.
            const int16_t* p = pcm.data();
            if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpcPos > (UINT64)t0_) {
                const uint64_t target = (uint64_t)(((int64_t)qpcPos - t0_) * (int64_t)kRate / 10000000LL);
                if (target > written_ + 480) {
                    EmitSilence(target - written_);
                } else if (target + 2400 < written_) {
                    const uint64_t drop = std::min<uint64_t>(written_ - target, outFrames);
                    droppedFrames_ += drop;
                    p += drop * 2;
                    outFrames -= (uint32_t)drop;
                }
            }
            Emit(p, outFrames);
        }
        // Endpoint idle: keep the timeline continuous with silence (100 ms behind now).
        const int64_t now = QpcNow100ns();
        if (now - lastPacketQpc > 1000000) {
            const int64_t idleTarget = (now - t0_) * (int64_t)kRate / 10000000LL - 4800;
            if (idleTarget > (int64_t)written_) EmitSilence((uint64_t)idleTarget - written_);
        }
    }
    client->Stop();
    {   // close the timeline up to the stop time so the audio track is as long as the video
        const int64_t endTarget = (QpcNow100ns() - t0_) * (int64_t)kRate / 10000000LL;
        if (endTarget > (int64_t)written_) EmitSilence((uint64_t)endTarget - written_);
    }

done:
    if (wfx) CoTaskMemFree(wfx);
    cap.Reset(); client.Reset(); dev.Reset(); en.Reset();
    if (SUCCEEDED(coHr)) CoUninitialize();
}
