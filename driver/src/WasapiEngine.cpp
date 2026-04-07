//-----------------------------------------------------------------------------
// WasapiEngine.cpp -- WASAPI Shared Mode audio backend (float32 throughout)
//-----------------------------------------------------------------------------
#include "WasapiEngine.h"
#include "GainProcessor.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <propvarutil.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cwchar>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "propsys.lib")

template<typename T>
static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static void OMEC_TRACE(const char* msg)
{
    OutputDebugStringA("[OmecWASAPI] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
static void OMEC_TRACEF(const char* fmt, ...)
{
    char buf[512]; va_list a; va_start(a,fmt); vsprintf_s(buf,fmt,a); va_end(a);
    OMEC_TRACE(buf);
}

static bool wcsContainsCI(const wchar_t* h, const wchar_t* n)
{
    if (!h||!n) return false;
    size_t hl=wcslen(h), nl=wcslen(n);
    if (nl>hl) return false;
    for (size_t i=0;i<=hl-nl;++i)
        if (_wcsnicmp(h+i,n,nl)==0) return true;
    return false;
}

// ---------------------------------------------------------------------------
WasapiEngine::WasapiEngine()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiEngine::~WasapiEngine()
{
    stop(); close();
    if (m_stopEvent) { CloseHandle(m_stopEvent); m_stopEvent = nullptr; }
}

// ---------------------------------------------------------------------------
bool WasapiEngine::open()
{
    OMEC_TRACE("open() called");
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enum);
    if (FAILED(hr)) { OMEC_TRACEF("CoCreate failed 0x%08X", hr); return false; }

    IMMDevice *cap = nullptr, *ren = nullptr;
    if (!findEndpoints(&cap, &ren))
    { OMEC_TRACE("findEndpoints: not found"); return false; }

    m_capDev = cap; m_renDev = ren;

    IAudioClient* tmp = nullptr;
    if (SUCCEEDED(cap->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&tmp)))
    {
        WAVEFORMATEX* mixFmt = nullptr;
        if (SUCCEEDED(tmp->GetMixFormat(&mixFmt)))
        {
            m_deviceSampleRate = mixFmt->nSamplesPerSec;
            OMEC_TRACEF("  Device mix format: %u Hz, %u-bit, %u ch, tag=0x%04X",
                        mixFmt->nSamplesPerSec, mixFmt->wBitsPerSample,
                        mixFmt->nChannels, mixFmt->wFormatTag);
            CoTaskMemFree(mixFmt);
        }
        tmp->Release();
    }

    m_open.store(true);
    OMEC_TRACE("open() OK");
    return true;
}

bool WasapiEngine::findEndpoints(IMMDevice** ppCap, IMMDevice** ppRen)
{
    static const wchar_t* kw[] =
        {L"OMEC", L"Teleport", L"Orange", L"08BB", L"08bb"};
    static const wchar_t* fb[] =
        {L"USB AUDIO", L"USB Audio", L"CODEC", L"Codec"};

    auto match = [&](IMMDevice* d) -> bool {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) {
            for (auto k : kw) if (wcsContainsCI(id, k)) { CoTaskMemFree(id); return true; }
            CoTaskMemFree(id);
        }
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            auto ck = [&](const PROPERTYKEY& pk) -> bool {
                PROPVARIANT v; PropVariantInit(&v); bool h = false;
                if (SUCCEEDED(ps->GetValue(pk, &v)) && v.vt == VT_LPWSTR) {
                    OMEC_TRACEF("  Endpoint: %S", v.pwszVal);
                    for (auto k : kw) if (wcsContainsCI(v.pwszVal, k)) { h = true; break; }
                } PropVariantClear(&v); return h;
            };
            bool h = ck(PKEY_Device_FriendlyName) || ck(PKEY_DeviceInterface_FriendlyName);
            ps->Release(); if (h) return true;
        } return false;
    };

    IMMDevice *fCap = nullptr, *fRen = nullptr; UINT fcC = 0, fcR = 0;
    auto search = [&](EDataFlow fl, IMMDevice** ex, IMMDevice** fb2, UINT* cnt) {
        IMMDeviceCollection* c = nullptr;
        if (FAILED(m_enum->EnumAudioEndpoints(fl, DEVICE_STATE_ACTIVE, &c))) return;
        UINT n = 0; c->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr; if (FAILED(c->Item(i, &d))) continue;
            if (!*ex && match(d)) { *ex = d; d = nullptr; }
            else {
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT v; PropVariantInit(&v); bool ok = false;
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                        for (auto f2 : fb) if (wcsContainsCI(v.pwszVal, f2)) { ok = true; break; }
                    PropVariantClear(&v); ps->Release();
                    if (ok) { (*cnt)++; if (*fb2)(*fb2)->Release(); *fb2 = d; d = nullptr; }
                }
            }
            if (d) d->Release();
        } c->Release();
    };

    search(eCapture, ppCap, &fCap, &fcC);
    search(eRender, ppRen, &fRen, &fcR);
    if (!*ppCap && fCap) { *ppCap = fCap; fCap = nullptr; }
    if (!*ppRen && fRen) { *ppRen = fRen; fRen = nullptr; }
    if (fCap) fCap->Release(); if (fRen) fRen->Release();
    if (!*ppCap) OMEC_TRACE("  No capture endpoint");
    if (!*ppRen) OMEC_TRACE("  No render endpoint");
    return (*ppCap != nullptr);
}

// ---------------------------------------------------------------------------
bool WasapiEngine::start(uint32_t sampleRate, uint32_t bufferSize)
{
    if (m_running.load() || !m_open.load()) return false;
    OMEC_TRACEF("start() sr=%u bs=%u (shared mode, float32)", sampleRate, bufferSize);

    // ---- Capture ----
    HRESULT hr = m_capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                     nullptr, (void**)&m_capAC);
    if (FAILED(hr)) { OMEC_TRACEF("Cap Activate fail 0x%08X", hr); return false; }

    WAVEFORMATEX* capMix = nullptr;
    m_capAC->GetMixFormat(&capMix);
    if (!capMix) { OMEC_TRACE("GetMixFormat failed"); return false; }

    OMEC_TRACEF("  Cap mix: %u Hz, %u-bit, %u ch, tag=0x%04X",
                capMix->nSamplesPerSec, capMix->wBitsPerSample,
                capMix->nChannels, capMix->wFormatTag);

    m_capBps = capMix->wBitsPerSample;
    m_capChannels = capMix->nChannels;
    m_capIsFloat = (capMix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (capMix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(capMix);
        m_capIsFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    m_capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = m_capAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              0, 0, capMix, nullptr);
    if (FAILED(hr)) {
        OMEC_TRACEF("Cap Init fail 0x%08X", hr);
        CoTaskMemFree(capMix); return false;
    }
    m_capAC->SetEventHandle(m_capEvent);
    m_capAC->GetService(__uuidof(IAudioCaptureClient), (void**)&m_capIC);
    m_capAC->GetBufferSize(&m_capBufFrames);
    OMEC_TRACEF("  Cap buffer: %u frames", m_capBufFrames);
    CoTaskMemFree(capMix);

    // ---- Render ----
    if (m_renDev) {
        hr = m_renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                 nullptr, (void**)&m_renAC);
        if (SUCCEEDED(hr)) {
            WAVEFORMATEX* renMix = nullptr;
            m_renAC->GetMixFormat(&renMix);
            if (renMix) {
                OMEC_TRACEF("  Ren mix: %u Hz, %u-bit, %u ch, tag=0x%04X",
                            renMix->nSamplesPerSec, renMix->wBitsPerSample,
                            renMix->nChannels, renMix->wFormatTag);
                m_renBps = renMix->wBitsPerSample;
                m_renChannels = renMix->nChannels;
                m_renIsFloat = (renMix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
                if (renMix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(renMix);
                    m_renIsFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                }
                hr = m_renAC->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                          0, 0, renMix, nullptr);
                if (SUCCEEDED(hr)) {
                    m_renAC->GetService(__uuidof(IAudioRenderClient), (void**)&m_renIC);
                    m_renAC->GetBufferSize(&m_renBufFrames);
                    OMEC_TRACEF("  Ren buffer: %u frames", m_renBufFrames);
                } else {
                    OMEC_TRACEF("Ren Init fail 0x%08X", hr);
                    safeRelease(m_renAC);
                }
                CoTaskMemFree(renMix);
            }
        }
    }

    m_capRing.clear();
    m_outRing.clear();
    ResetEvent(m_stopEvent);
    m_running.store(true);
    m_thread = std::thread([this] { audioThreadProc(); });
    return true;
}

void WasapiEngine::stop()
{
    if (!m_running.load()) return;
    m_running.store(false);
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
    if (m_capAC) m_capAC->Stop();
    if (m_renAC) m_renAC->Stop();
    safeRelease(m_capIC); safeRelease(m_renIC);
    safeRelease(m_capAC); safeRelease(m_renAC);
    if (m_capEvent) { CloseHandle(m_capEvent); m_capEvent = nullptr; }
}

void WasapiEngine::close()
{
    stop();
    safeRelease(m_capDev); safeRelease(m_renDev); safeRelease(m_enum);
    m_open.store(false);
}

// ---------------------------------------------------------------------------
// audioThreadProc -- float32 end-to-end, no int16 conversion anywhere
// ---------------------------------------------------------------------------
void WasapiEngine::audioThreadProc()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD ti = 0;
    HANDLE task = AvSetMmThreadCharacteristicsA("Pro Audio", &ti);

    __try
    {
        if (m_renAC) m_renAC->Start();
        m_capAC->Start();

        HANDLE wh[2] = { m_capEvent, m_stopEvent };
        const uint32_t asioBufSamples = m_asioBufSize * 2; // stereo float

        while (m_running.load())
        {
            DWORD r = WaitForMultipleObjects(2, wh, FALSE, 100);
            if (r == WAIT_OBJECT_0 + 1 || !m_running.load()) break;
            if (r != WAIT_OBJECT_0) continue;

            // ---- Read all available capture data into ring ----
            UINT32 pktSz = 0;
            while (SUCCEEDED(m_capIC->GetNextPacketSize(&pktSz)) && pktSz > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(m_capIC->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;

                uint32_t stereoSamples = frames * 2;
                if (m_capRing.freeSpace() >= stereoSamples)
                {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                    {
                        memset(m_tmpF32, 0, stereoSamples * sizeof(float));
                        m_capRing.write(m_tmpF32, stereoSamples);
                    }
                    else if (m_capIsFloat && m_capBps == 32)
                    {
                        // Float32 — extract stereo pair directly
                        const float* src = reinterpret_cast<const float*>(data);
                        if (m_capChannels == 2)
                        {
                            // Direct passthrough — no conversion needed
                            m_capRing.write(src, stereoSamples);
                        }
                        else
                        {
                            // Multi-channel or mono → extract L/R
                            for (uint32_t i = 0; i < frames; ++i)
                            {
                                m_tmpF32[i * 2 + 0] = src[i * m_capChannels + 0];
                                m_tmpF32[i * 2 + 1] = (m_capChannels >= 2)
                                    ? src[i * m_capChannels + 1]
                                    : src[i * m_capChannels + 0];
                            }
                            m_capRing.write(m_tmpF32, stereoSamples);
                        }
                    }
                    else if (!m_capIsFloat && m_capBps == 16)
                    {
                        // 16-bit PCM → float (unlikely in shared mode, but safe)
                        const int16_t* src = reinterpret_cast<const int16_t*>(data);
                        for (uint32_t i = 0; i < frames; ++i)
                        {
                            m_tmpF32[i * 2 + 0] = src[i * m_capChannels + 0] / 32768.f;
                            m_tmpF32[i * 2 + 1] = (m_capChannels >= 2)
                                ? src[i * m_capChannels + 1] / 32768.f
                                : src[i * m_capChannels + 0] / 32768.f;
                        }
                        m_capRing.write(m_tmpF32, stereoSamples);
                    }
                    else
                    {
                        memset(m_tmpF32, 0, stereoSamples * sizeof(float));
                        m_capRing.write(m_tmpF32, stereoSamples);
                    }
                }
                m_capIC->ReleaseBuffer(frames);
            }

            // ---- Drift correction: discard excess capture data ----
            // USB audio timing drifts over time, causing the ring to grow.
            // Allow up to the full WASAPI buffer size in the ring. Only trim
            // when drift has accumulated beyond that — roughly 20ms at 48kHz.
            {
                const uint32_t maxAllowed = m_capBufFrames * 2; // stereo samples
                uint32_t avail = m_capRing.available();
                if (avail > maxAllowed)
                {
                    float discard[MAX_BUF * 2];
                    uint32_t target = asioBufSamples; // trim down to 1 ASIO buffer
                    while (m_capRing.available() > target)
                    {
                        uint32_t toDrop = std::min(m_capRing.available() - target,
                                                   (uint32_t)(MAX_BUF * 2));
                        m_capRing.read(discard, toDrop);
                    }
                }
            }

            // ---- Drain capture ring → ASIO callbacks ----
            while (m_capRing.available() >= asioBufSamples && m_running.load())
            {
                float capBuf[MAX_BUF * 2];
                float renBuf[MAX_BUF * 2];
                memset(renBuf, 0, asioBufSamples * sizeof(float));

                m_capRing.read(capBuf, asioBufSamples);

                // Feed calibration BEFORE gain — measures raw input level
                if (m_gain && m_gain->isCalibrationActive())
                    m_gain->feedCalibration(capBuf, static_cast<int>(m_asioBufSize),
                                            m_deviceSampleRate);

                if (m_gain)
                    m_gain->processInput(capBuf, static_cast<int>(m_asioBufSize));

                if (m_asioFunc)
                    m_asioFunc(m_asioCtx, capBuf, renBuf, m_asioBufSize);

                // Apply soft limiter to output before sending to hardware
                if (m_gain)
                    m_gain->applySoftLimiter(renBuf, static_cast<int>(m_asioBufSize));

                if (m_outRing.freeSpace() >= asioBufSamples)
                    m_outRing.write(renBuf, asioBufSamples);
            }

            // ---- Write output ring → WASAPI render ----
            if (m_renIC && m_renAC)
            {
                UINT32 padding = 0;
                m_renAC->GetCurrentPadding(&padding);
                UINT32 available = m_renBufFrames - padding;

                uint32_t outFrames = m_outRing.available() / 2;
                uint32_t toWrite = std::min(outFrames, available);

                if (toWrite > 0)
                {
                    BYTE* renData = nullptr;
                    if (SUCCEEDED(m_renIC->GetBuffer(toWrite, &renData)))
                    {
                        if (m_renIsFloat && m_renBps == 32 && m_renChannels == 2)
                        {
                            // Direct float stereo passthrough
                            float* dst = reinterpret_cast<float*>(renData);
                            m_outRing.read(dst, toWrite * 2);
                        }
                        else if (m_renIsFloat && m_renBps == 32)
                        {
                            // Multi-channel float — write L/R, zero others
                            m_outRing.read(m_tmpF32, toWrite * 2);
                            float* dst = reinterpret_cast<float*>(renData);
                            for (uint32_t i = 0; i < toWrite; ++i)
                            {
                                dst[i * m_renChannels + 0] = m_tmpF32[i * 2 + 0];
                                if (m_renChannels >= 2)
                                    dst[i * m_renChannels + 1] = m_tmpF32[i * 2 + 1];
                                for (WORD c = 2; c < m_renChannels; ++c)
                                    dst[i * m_renChannels + c] = 0.f;
                            }
                        }
                        else
                        {
                            // Fallback: silence
                            m_outRing.read(m_tmpF32, toWrite * 2); // drain ring
                            memset(renData, 0, toWrite * m_renChannels * (m_renBps / 8));
                        }
                        m_renIC->ReleaseBuffer(toWrite, 0);
                    }
                }
            }
        }

        m_capAC->Stop();
        if (m_renAC) m_renAC->Stop();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        OMEC_TRACE("EXCEPTION in audioThreadProc!");
        m_running.store(false);
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
}
