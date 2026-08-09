//-----------------------------------------------------------------------------
// WasapiEngine.cpp -- WASAPI Shared Mode audio backend (float32 throughout)
//
// v1.6 audio-path overhaul:
//  - The stream runs at the host's ASIO rate via AUDCLNT_STREAMFLAGS_
//    AUTOCONVERTPCM (the Windows engine resamples), so the rate the host
//    sets is the rate that flows — no more silent mix-rate mismatch.
//  - Honest latency accounting (WASAPI buffers + ring residency).
//  - Output clock-drift is corrected by micro time-compression with a
//    short crossfade (writeOutputBuffer) instead of splicing ~22 ms chunks.
//  - Capture-ring overflow drops OLDEST audio (counted), not newest.
//  - Every HRESULT on the setup path is checked; failure paths release
//    what they created; stop() always joins the audio thread.
//  - Device removal / engine faults are detected and reported to the owner
//    via the reset-request callback instead of freezing silently.
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

// Device-name portion used to pair capture/render endpoints of one physical
// device: "Speakers (USB Audio CODEC)" -> "USB Audio CODEC".  Falls back to
// the whole name when there are no parentheses.
static void extractDevicePart(const wchar_t* name, wchar_t* out, size_t outLen)
{
    out[0] = L'\0';
    if (!name) return;
    // FIRST '(' paired with LAST ')' so nested parens keep the full device
    // part: "Mic (AcmeCo(TM) USB Audio)" -> "AcmeCo(TM) USB Audio".
    const wchar_t* open  = wcschr(name, L'(');
    const wchar_t* close = open ? wcsrchr(name, L')') : nullptr;
    if (open && close && close > open + 1)
    {
        size_t n = std::min<size_t>(close - open - 1, outLen - 1);
        wcsncpy_s(out, outLen, open + 1, n);
    }
    else
    {
        wcsncpy_s(out, outLen, name, _TRUNCATE);
    }
}

// Copy a WAVEFORMATEX(TENSIBLE) into 'buf' with the sample rate replaced.
// Channel count / bit depth are untouched — AUTOCONVERTPCM only needs the
// rate changed to make the engine resample to/from our rate.
static WAVEFORMATEX* copyFormatWithRate(const WAVEFORMATEX* src, uint32_t rate,
                                        BYTE* buf, size_t bufLen)
{
    const size_t total = sizeof(WAVEFORMATEX) + src->cbSize;
    if (total > bufLen) return nullptr;
    std::memcpy(buf, src, total);
    auto* f = reinterpret_cast<WAVEFORMATEX*>(buf);
    f->nSamplesPerSec  = rate;
    f->nAvgBytesPerSec = rate * f->nBlockAlign;
    return f;
}

static void cacheFormatInfo(const WAVEFORMATEX* fmt,
                            WORD& bps, WORD& channels, bool& isFloat)
{
    bps      = fmt->wBitsPerSample;
    channels = fmt->nChannels;
    isFloat  = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
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
    if (!m_enum)
    {
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enum);
        if (FAILED(hr)) { OMEC_TRACEF("CoCreate failed 0x%08X", hr); return false; }
    }

    // Re-opening: drop previous endpoints first.
    safeRelease(m_capDev);
    safeRelease(m_renDev);

    IMMDevice *cap = nullptr, *ren = nullptr;
    if (!findEndpoints(&cap, &ren))
    { OMEC_TRACE("findEndpoints: not found"); return false; }

    m_capDev = cap; m_renDev = ren;

    IAudioClient* tmp = nullptr;
    if (SUCCEEDED(cap->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&tmp)))
    {
        WAVEFORMATEX* mixFmt = nullptr;
        if (SUCCEEDED(tmp->GetMixFormat(&mixFmt)) && mixFmt)
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

    auto friendlyName = [](IMMDevice* d, wchar_t* out, size_t outLen) {
        out[0] = L'\0';
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)))
        {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                wcsncpy_s(out, outLen, v.pwszVal, _TRUNCATE);
            PropVariantClear(&v); ps->Release();
        }
    };

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

    // Per-flow search: exact keyword match wins; otherwise remember generic
    // "USB Audio"-style candidates, counting how many distinct ones exist.
    IMMDevice *fCap = nullptr, *fRen = nullptr;
    UINT fcC = 0, fcR = 0;
    wchar_t capName[256] = {}, renName[256] = {};
    wchar_t fCapName[256] = {}, fRenName[256] = {};

    auto search = [&](EDataFlow fl, IMMDevice** ex, IMMDevice** fb2, UINT* cnt,
                       wchar_t* exName, wchar_t* fbName) {
        IMMDeviceCollection* c = nullptr;
        if (FAILED(m_enum->EnumAudioEndpoints(fl, DEVICE_STATE_ACTIVE, &c))) return;
        UINT n = 0; c->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr; if (FAILED(c->Item(i, &d))) continue;
            if (!*ex && match(d)) { friendlyName(d, exName, 256); *ex = d; d = nullptr; }
            else {
                wchar_t name[256] = {};
                friendlyName(d, name, 256);
                bool ok = false;
                for (auto f2 : fb) if (wcsContainsCI(name, f2)) { ok = true; break; }
                if (ok) {
                    (*cnt)++;
                    if (*fb2) (*fb2)->Release();
                    *fb2 = d; d = nullptr;
                    wcsncpy_s(fbName, 256, name, _TRUNCATE);
                }
            }
            if (d) d->Release();
        } c->Release();
    };

    search(eCapture, ppCap, &fCap, &fcC, capName, fCapName);
    search(eRender,  ppRen, &fRen, &fcR, renName, fRenName);

    // Capture fallback: only accept an unambiguous single candidate.  Two
    // generic "USB Audio CODEC" boxes means we cannot know which is the
    // Teleport — fail loudly rather than open the wrong hardware.
    if (!*ppCap && fCap)
    {
        if (fcC == 1) { *ppCap = fCap; fCap = nullptr; wcscpy_s(capName, fCapName); }
        else OMEC_TRACEF("  Capture fallback ambiguous (%u candidates) — rejecting", fcC);
    }

    // Render fallback: must be unambiguous AND belong to the same physical
    // device as the chosen capture endpoint (paired by the parenthesised
    // device-name part), so we never split input/output across two boxes.
    if (!*ppRen && fRen)
    {
        wchar_t capPart[256], renPart[256];
        extractDevicePart(capName,  capPart, 256);
        extractDevicePart(fRenName, renPart, 256);
        if (fcR == 1 && capPart[0] && _wcsicmp(capPart, renPart) == 0)
        { *ppRen = fRen; fRen = nullptr; }
        else
            OMEC_TRACEF("  Render fallback rejected (candidates=%u, pair='%S' vs '%S')",
                        fcR, capPart, renPart);
    }

    if (fCap) fCap->Release();
    if (fRen) fRen->Release();

    if (!*ppCap)
    {
        OMEC_TRACE("  No capture endpoint");
        // Don't leak (or half-use) a render endpoint when capture is missing.
        if (*ppRen) { (*ppRen)->Release(); *ppRen = nullptr; }
        return false;
    }
    if (!*ppRen) OMEC_TRACE("  No render endpoint (input-only mode)");
    return true;
}

// ---------------------------------------------------------------------------
void WasapiEngine::releaseStreamObjects()
{
    safeRelease(m_capIC); safeRelease(m_renIC);
    safeRelease(m_capAC); safeRelease(m_renAC);
    if (m_capEvent) { CloseHandle(m_capEvent); m_capEvent = nullptr; }
}

void WasapiEngine::fatalStreamError(const char* msg)
{
    OMEC_TRACEF("FATAL stream error: %s", msg);
    m_fatalError.store(true, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    // The device may be gone entirely — force a fresh endpoint enumeration
    // on the next start() so recovery works after replugging, instead of
    // Activate() failing forever on a stale IMMDevice.
    m_open.store(false, std::memory_order_release);
    if (m_resetFunc)
        m_resetFunc(m_resetCtx);   // ask the host to reset us (async request)
}

// ---------------------------------------------------------------------------
bool WasapiEngine::start(uint32_t sampleRate, uint32_t bufferSize)
{
    if (m_running.load() || !m_open.load()) return false;
    if (m_thread.joinable())
    {
        // Reap a thread killed by a fatal error — but never join ourselves
        // (a host reacting to kAsioResetRequest synchronously could re-enter
        // start() on the audio thread; joining self throws).
        if (m_thread.get_id() == std::this_thread::get_id())
            return false;
        m_thread.join();
    }
    // Release any stream objects a fatal-error session left behind (the
    // host's stop may have been short-circuited by the fatal path).
    releaseStreamObjects();
    OMEC_TRACEF("start() sr=%u bs=%u (shared mode, float32)", sampleRate, bufferSize);

    m_fatalError.store(false, std::memory_order_release);

    // ---- Capture ----
    HRESULT hr = m_capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                     nullptr, (void**)&m_capAC);
    if (FAILED(hr)) { OMEC_TRACEF("Cap Activate fail 0x%08X", hr); return false; }

    WAVEFORMATEX* capMix = nullptr;
    hr = m_capAC->GetMixFormat(&capMix);
    if (FAILED(hr) || !capMix)
    { OMEC_TRACE("Cap GetMixFormat failed"); releaseStreamObjects(); return false; }

    OMEC_TRACEF("  Cap mix: %u Hz, %u-bit, %u ch, tag=0x%04X",
                capMix->nSamplesPerSec, capMix->wBitsPerSample,
                capMix->nChannels, capMix->wFormatTag);
    cacheFormatInfo(capMix, m_capBps, m_capChannels, m_capIsFloat);

    m_capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_capEvent)
    { CoTaskMemFree(capMix); releaseStreamObjects(); return false; }

    // Run the stream at the host's ASIO rate: AUTOCONVERTPCM makes the
    // Windows engine resample between the endpoint mix rate and our rate,
    // so the rate the host set is the rate that actually flows.
    uint32_t capRate = 0;
    {
        BYTE fmtBuf[128];
        WAVEFORMATEX* req = copyFormatWithRate(capMix, sampleRate, fmtBuf, sizeof(fmtBuf));
        const DWORD convFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = req ? m_capAC->Initialize(AUDCLNT_SHAREMODE_SHARED, convFlags,
                                       0, 0, req, nullptr)
                 : E_FAIL;
        if (SUCCEEDED(hr))
            capRate = sampleRate;
        else
        {
            OMEC_TRACEF("Cap Init @%u Hz fail 0x%08X — falling back to mix rate",
                        sampleRate, hr);
            // A failed Initialize can leave the client unusable — get a fresh one.
            safeRelease(m_capAC);
            hr = m_capDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                     nullptr, (void**)&m_capAC);
            if (SUCCEEDED(hr))
                hr = m_capAC->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          0, 0, capMix, nullptr);
            if (FAILED(hr))
            {
                OMEC_TRACEF("Cap Init fail 0x%08X", hr);
                CoTaskMemFree(capMix); releaseStreamObjects(); return false;
            }
            capRate = capMix->nSamplesPerSec;
        }
    }
    CoTaskMemFree(capMix);

    if (FAILED(m_capAC->SetEventHandle(m_capEvent)) ||
        FAILED(m_capAC->GetService(__uuidof(IAudioCaptureClient), (void**)&m_capIC)) ||
        FAILED(m_capAC->GetBufferSize(&m_capBufFrames)) ||
        m_capBufFrames == 0)
    {
        OMEC_TRACE("Cap SetEventHandle/GetService/GetBufferSize failed");
        releaseStreamObjects(); return false;
    }
    OMEC_TRACEF("  Cap buffer: %u frames @ %u Hz", m_capBufFrames, capRate);
    m_deviceSampleRate = capRate;

    // ---- Render (optional — engine degrades to input-only) ----
    m_renBufFrames = 0;
    if (m_renDev)
    {
        hr = m_renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                 nullptr, (void**)&m_renAC);
        if (SUCCEEDED(hr))
        {
            WAVEFORMATEX* renMix = nullptr;
            hr = m_renAC->GetMixFormat(&renMix);
            if (SUCCEEDED(hr) && renMix)
            {
                OMEC_TRACEF("  Ren mix: %u Hz, %u-bit, %u ch, tag=0x%04X",
                            renMix->nSamplesPerSec, renMix->wBitsPerSample,
                            renMix->nChannels, renMix->wFormatTag);
                cacheFormatInfo(renMix, m_renBps, m_renChannels, m_renIsFloat);

                uint32_t renRate = 0;
                BYTE fmtBuf[128];
                WAVEFORMATEX* req = copyFormatWithRate(renMix, capRate, fmtBuf, sizeof(fmtBuf));
                const DWORD convFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                hr = req ? m_renAC->Initialize(AUDCLNT_SHAREMODE_SHARED, convFlags,
                                               0, 0, req, nullptr)
                         : E_FAIL;
                if (SUCCEEDED(hr))
                    renRate = capRate;
                else
                {
                    OMEC_TRACEF("Ren Init @%u Hz fail 0x%08X — falling back to mix rate",
                                capRate, hr);
                    safeRelease(m_renAC);
                    if (SUCCEEDED(m_renDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                                      nullptr, (void**)&m_renAC)))
                        hr = m_renAC->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                                  0, 0, renMix, nullptr);
                    renRate = renMix->nSamplesPerSec;
                }

                if (SUCCEEDED(hr) && m_renAC)
                {
                    // Refuse a render path at a different rate than capture —
                    // that means constant pitch error and unbounded drift.
                    if (renRate != capRate ||
                        FAILED(m_renAC->GetService(__uuidof(IAudioRenderClient), (void**)&m_renIC)) ||
                        FAILED(m_renAC->GetBufferSize(&m_renBufFrames)) ||
                        m_renBufFrames == 0)
                    {
                        if (renRate != capRate)
                            OMEC_TRACEF("  Render rate %u != capture rate %u — disabling render",
                                        renRate, capRate);
                        else
                            OMEC_TRACE("  Ren GetService/GetBufferSize failed — disabling render");
                        safeRelease(m_renIC); safeRelease(m_renAC);
                        m_renBufFrames = 0;
                    }
                    else
                    {
                        OMEC_TRACEF("  Ren buffer: %u frames @ %u Hz", m_renBufFrames, renRate);
                    }
                }
                else
                {
                    safeRelease(m_renAC);
                }
                CoTaskMemFree(renMix);
            }
            else
            {
                if (renMix) CoTaskMemFree(renMix);
                safeRelease(m_renAC);
            }
        }
    }

    // Pre-fill render with silence so playback starts without an underrun.
    if (m_renAC && m_renIC)
    {
        BYTE* p = nullptr;
        if (SUCCEEDED(m_renIC->GetBuffer(m_renBufFrames, &p)))
            m_renIC->ReleaseBuffer(m_renBufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    // ---- Start the streams (checked) ----
    hr = m_capAC->Start();
    if (FAILED(hr))
    { OMEC_TRACEF("Cap Start fail 0x%08X", hr); releaseStreamObjects(); return false; }
    if (m_renAC)
    {
        hr = m_renAC->Start();
        if (FAILED(hr))
        {
            OMEC_TRACEF("Ren Start fail 0x%08X — input-only", hr);
            safeRelease(m_renIC); safeRelease(m_renAC);
            m_renBufFrames = 0;
        }
    }

    // Steady-state output backlog target and honest latency figures —
    // computed AFTER the Start calls so a render-start failure can't leave
    // phantom render terms in the reported latency.  Output residency in
    // the ring rests between ~0 (fresh) and ~target (post-drift, decayed by
    // the tier-2 trim), so target/2 is the representative ring term; render
    // padding contributes its full buffer.
    m_outTargetFrames = (m_renAC ? std::min<uint32_t>(m_renBufFrames, 2048u) : 0);
    m_inLatencyFrames.store(m_capBufFrames + m_asioBufSize, std::memory_order_release);
    m_outLatencyFrames.store(m_asioBufSize + m_renBufFrames + m_outTargetFrames / 2,
                             std::memory_order_release);

    m_capRing.clear();
    m_outRing.clear();
    m_capOverflowCount.store(0);
    m_outTrimCount.store(0);
    m_outHardTrimCount.store(0);
    ResetEvent(m_stopEvent);
    m_running.store(true);
    m_thread = std::thread([this] { audioThreadProc(); });
    return true;
}

void WasapiEngine::stop()
{
    // Always join and clean up, even after a fatal error already cleared
    // m_running — otherwise a later start() move-assigns onto a joinable
    // thread and std::terminate takes down the host.
    m_running.store(false);
    if (m_stopEvent) SetEvent(m_stopEvent);
    // Never join from the audio thread itself (possible when a host reacts
    // to kAsioResetRequest synchronously inside the callback): the thread
    // is already on its way out, and start() reaps the joinable handle.
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id())
        m_thread.join();
    if (m_capAC) m_capAC->Stop();
    if (m_renAC) m_renAC->Stop();
    releaseStreamObjects();
}

void WasapiEngine::close()
{
    stop();
    safeRelease(m_capDev); safeRelease(m_renDev); safeRelease(m_enum);
    m_open.store(false);
}

// ---------------------------------------------------------------------------
// Capture packet -> capture ring, converting to interleaved stereo float.
// Oversized packets are converted in MAX_BUF chunks (m_tmpF32 is the only
// scratch buffer).  On overflow the OLDEST ring audio is dropped so the
// freshest input survives.  Audio thread only.
// ---------------------------------------------------------------------------
void WasapiEngine::writeCapturePacket(const BYTE* data, uint32_t frames, DWORD flags)
{
    const uint32_t stereoSamples = frames * 2;
    if (stereoSamples > StereoRing::CAPACITY)
        return;   // pathological packet; nothing sane to do

    if (m_capRing.freeSpace() < stereoSamples)
    {
        m_capRing.discard(stereoSamples - m_capRing.freeSpace());
        m_capOverflowCount.fetch_add(1, std::memory_order_relaxed);
    }

    // Fast path: already interleaved stereo float — straight into the ring.
    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) &&
        m_capIsFloat && m_capBps == 32 && m_capChannels == 2)
    {
        m_capRing.write(reinterpret_cast<const float*>(data), stereoSamples);
        return;
    }

    uint32_t done = 0;
    while (done < frames)
    {
        const uint32_t n = std::min<uint32_t>(frames - done, MAX_BUF);

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
            std::memset(m_tmpF32, 0, n * 2 * sizeof(float));
        }
        else if (m_capIsFloat && m_capBps == 32)
        {
            const float* src = reinterpret_cast<const float*>(data)
                             + static_cast<size_t>(done) * m_capChannels;
            for (uint32_t i = 0; i < n; ++i)
            {
                m_tmpF32[i * 2 + 0] = src[i * m_capChannels + 0];
                m_tmpF32[i * 2 + 1] = (m_capChannels >= 2)
                    ? src[i * m_capChannels + 1]
                    : src[i * m_capChannels + 0];
            }
        }
        else if (!m_capIsFloat && m_capBps == 16)
        {
            const int16_t* src = reinterpret_cast<const int16_t*>(data)
                               + static_cast<size_t>(done) * m_capChannels;
            for (uint32_t i = 0; i < n; ++i)
            {
                m_tmpF32[i * 2 + 0] = src[i * m_capChannels + 0] / 32768.f;
                m_tmpF32[i * 2 + 1] = (m_capChannels >= 2)
                    ? src[i * m_capChannels + 1] / 32768.f
                    : src[i * m_capChannels + 0] / 32768.f;
            }
        }
        else
        {
            std::memset(m_tmpF32, 0, n * 2 * sizeof(float));
        }

        m_capRing.write(m_tmpF32, n * 2);
        done += n;
    }
}

// ---------------------------------------------------------------------------
// Rendered ASIO buffer -> output ring, with clock-drift control.
//
// When the output backlog creeps above its steady-state target (the two
// stream clocks are never perfectly matched, and heavy host DSP can burst),
// we shorten this buffer by a few frames using a short crossfade before
// writing it — micro time-compression.  Each event removes at most
// asioBufSize/8 frames with a 32-frame blend, which is inaudible, instead
// of the old behaviour of splicing out ~22 ms in one audible click.
// Audio thread only.
// ---------------------------------------------------------------------------
void WasapiEngine::writeOutputBuffer(float* renBuf)
{
    // Input-only mode: nothing drains the output ring — don't fill it.
    if (!m_renIC)
        return;

    const uint32_t N = m_asioBufSize;
    uint32_t framesToWrite = N;

    if (m_outTargetFrames > 0)
    {
        const uint32_t backlogFrames = m_outRing.available() / 2;
        const uint32_t ceiling = m_outTargetFrames + m_outTargetFrames / 2 + N;

        // Two trim tiers so residency decays back to target instead of
        // ratcheting up to the ceiling after a burst:
        //   tier 1 (backlog > ceiling): fast, N/8 frames per callback
        //   tier 2 (backlog > target):  gentle, N/64 frames per callback
        uint32_t maxDrop = 0;
        if (backlogFrames > ceiling)
            maxDrop = std::max(1u, N / 8);
        else if (backlogFrames > m_outTargetFrames)
            maxDrop = std::max(1u, N / 64);

        if (maxDrop > 0)
        {
            const uint32_t excess  = backlogFrames - m_outTargetFrames;
            uint32_t drop = std::min(excess, maxDrop);
            drop = std::min(drop, N / 2);          // never eat most of a buffer
            const uint32_t K = N - drop;           // frames kept
            const uint32_t F = std::min<uint32_t>(32, K);  // crossfade length
            if (drop > 0 && K >= 8)
            {
                // Blend the kept tail into the true tail so the final frame
                // matches the original final frame (continuity into the
                // next callback).  Ascending in-place is safe: each source
                // index is read before any later iteration overwrites it.
                for (uint32_t j = 0; j < F; ++j)
                {
                    const float w = static_cast<float>(j + 1) / static_cast<float>(F);
                    const uint32_t dst = K - F + j;
                    const uint32_t src = N - F + j;
                    renBuf[dst * 2 + 0] = renBuf[dst * 2 + 0] * (1.0f - w)
                                        + renBuf[src * 2 + 0] * w;
                    renBuf[dst * 2 + 1] = renBuf[dst * 2 + 1] * (1.0f - w)
                                        + renBuf[src * 2 + 1] * w;
                }
                framesToWrite = K;
                m_outTrimCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const uint32_t samples = framesToWrite * 2;
    if (m_outRing.freeSpace() < samples)
    {
        // Emergency only (ring nearly full — render side stalled): drop the
        // oldest audio to make room.  Audible, but keeps the stream alive
        // and bounds latency; counted for diagnostics.
        m_outRing.discard(samples - m_outRing.freeSpace());
        m_outHardTrimCount.fetch_add(1, std::memory_order_relaxed);
    }
    m_outRing.write(renBuf, samples);
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
        HANDLE wh[2] = { m_capEvent, m_stopEvent };
        const uint32_t asioBufSamples = m_asioBufSize * 2; // stereo float
        uint32_t idleTimeouts = 0;

        while (m_running.load())
        {
            DWORD r = WaitForMultipleObjects(2, wh, FALSE, 100);
            if (r == WAIT_OBJECT_0 + 1 || !m_running.load()) break;
            if (r == WAIT_FAILED)
            { fatalStreamError("WaitForMultipleObjects failed"); break; }
            if (r != WAIT_OBJECT_0)
            {
                // No capture events for 5 s while running: the device is
                // gone or the engine died.  Tell the host instead of
                // freezing bufferSwitch forever.
                if (++idleTimeouts >= 50)
                { fatalStreamError("capture stalled — device removed?"); break; }
                continue;
            }
            idleTimeouts = 0;

            // ---- Read all available capture data into ring ----
            UINT32 pktSz = 0;
            HRESULT phr = S_OK;
            while (SUCCEEDED(phr = m_capIC->GetNextPacketSize(&pktSz)) && pktSz > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                HRESULT bhr = m_capIC->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(bhr))
                {
                    if (bhr == AUDCLNT_E_DEVICE_INVALIDATED)
                        fatalStreamError("capture device invalidated");
                    break;
                }
                writeCapturePacket(data, frames, flags);
                m_capIC->ReleaseBuffer(frames);
            }
            if (FAILED(phr) && phr == AUDCLNT_E_DEVICE_INVALIDATED)
                fatalStreamError("capture device invalidated");
            if (!m_running.load()) break;

            // ---- Drain capture ring → ASIO callbacks → output ring ----
            while (m_capRing.available() >= asioBufSamples && m_running.load())
            {
                float capBuf[MAX_BUF * 2];
                float renBuf[MAX_BUF * 2];
                memset(renBuf, 0, asioBufSamples * sizeof(float));

                m_capRing.read(capBuf, asioBufSamples);

                // Feed calibration BEFORE gain — measures raw input level.
                // (Lock-free: see GainProcessor::feedCalibration.)
                if (m_gain && m_gain->isCalibrationActive())
                    m_gain->feedCalibration(capBuf, static_cast<int>(m_asioBufSize),
                                            m_deviceSampleRate);

                if (m_gain)
                    m_gain->processInput(capBuf, static_cast<int>(m_asioBufSize));

                if (m_asioFunc)
                    m_asioFunc(m_asioCtx, capBuf, renBuf, m_asioBufSize);

                if (m_gain)
                {
                    // Output volume + output meters, then the limiter as the
                    // final stage so the ceiling applies to the volume-scaled
                    // signal.
                    m_gain->processOutput(renBuf, static_cast<int>(m_asioBufSize));
                    m_gain->applySoftLimiter(renBuf, static_cast<int>(m_asioBufSize));
                }

                writeOutputBuffer(renBuf);
            }

            // ---- Write output ring → WASAPI render ----
            if (m_renIC && m_renAC)
            {
                for (;;)
                {
                    UINT32 padding = 0;
                    HRESULT rhr = m_renAC->GetCurrentPadding(&padding);
                    if (FAILED(rhr))
                    {
                        if (rhr == AUDCLNT_E_DEVICE_INVALIDATED)
                            fatalStreamError("render device invalidated");
                        break;
                    }
                    UINT32 renderAvail = (padding <= m_renBufFrames)
                                       ? m_renBufFrames - padding : 0;
                    uint32_t outFrames = m_outRing.available() / 2;
                    uint32_t toWrite = std::min(outFrames, renderAvail);
                    if (toWrite == 0) break;

                    // Conversion paths stage through m_tmpF32 — clamp to it.
                    // (The loop comes back around for any remainder.)
                    if (!(m_renIsFloat && m_renBps == 32 && m_renChannels == 2))
                        toWrite = std::min<uint32_t>(toWrite, MAX_BUF);

                    BYTE* renData = nullptr;
                    if (FAILED(m_renIC->GetBuffer(toWrite, &renData))) break;

                    if (m_renIsFloat && m_renBps == 32 && m_renChannels == 2)
                    {
                        float* dst = reinterpret_cast<float*>(renData);
                        m_outRing.read(dst, toWrite * 2);
                    }
                    else if (m_renIsFloat && m_renBps == 32)
                    {
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

        if (m_capAC) m_capAC->Stop();
        if (m_renAC) m_renAC->Stop();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Never let a fault on the audio thread take the host down — die
        // gracefully and ask the host to reset the driver.
        OMEC_TRACE("EXCEPTION in audioThreadProc!");
        fatalStreamError("unhandled exception on audio thread");
    }

    if (task) AvRevertMmThreadCharacteristics(task);
    CoUninitialize();
}
