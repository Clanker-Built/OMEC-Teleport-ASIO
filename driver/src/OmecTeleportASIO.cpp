//-----------------------------------------------------------------------------
// OmecTeleportASIO.cpp — Main ASIO driver implementation
//-----------------------------------------------------------------------------

#include "OmecTeleportASIO.h"
#include "resource.h"
#include <initguid.h>   // must precede Guids.h to emit DEFINE_GUID definitions
#include "Guids.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdarg>

static void OMEC_TRACEF(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);
    OutputDebugStringA("[OmecASIO] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ============================================================================
// COM factory
// ============================================================================

CUnknown* OmecTeleportASIO::CreateInstance(LPUNKNOWN pUnk, HRESULT* phr)
{
    return reinterpret_cast<CUnknown*>(new OmecTeleportASIO(pUnk, phr));
}

HRESULT STDMETHODCALLTYPE OmecTeleportASIO::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    if (riid == CLSID_OmecTeleportASIO)
        return GetInterface(this, ppv);
    return CUnknown::NonDelegatingQueryInterface(riid, ppv);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

OmecTeleportASIO::OmecTeleportASIO(LPUNKNOWN pUnk, HRESULT* phr)
    : CUnknown(const_cast<TCHAR*>(TEXT("OmecTeleportASIO")), pUnk, phr)
{
    m_outputReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    m_usb      = std::make_unique<WasapiEngine>();
    m_gain     = std::make_unique<GainProcessor>();
    m_registry = std::make_unique<RegistrySettings>();
    m_panel    = std::make_unique<ControlPanel>();

    // Load persisted settings
    m_registry->load(m_settings);

    // Apply saved gain settings
    m_gain->setInputGainDB_L(m_settings.inputGainDB_L);
    m_gain->setInputGainDB_R(m_settings.inputGainDB_R);
    m_gain->setOutputVolumeDB(m_settings.outputVolumeDB);
    m_gain->setSoftLimiterEnabled(m_settings.softLimiterEnabled);

    m_sampleRate.store(static_cast<double>(m_settings.sampleRate));
    m_bufferSize = static_cast<long>(m_settings.bufferSize);
    m_bufferSizeAtomic.store(m_bufferSize);

    std::strcpy(m_errorMsg, "No error");
}

OmecTeleportASIO::~OmecTeleportASIO()
{
    stop();
    disposeBuffers();
    m_usb->close();

    if (m_outputReadyEvent)
    {
        CloseHandle(m_outputReadyEvent);
        m_outputReadyEvent = nullptr;
    }
}

// ============================================================================
// ASIOInit
// ============================================================================

ASIOBool OmecTeleportASIO::init(void* sysHandle)
{
    if (m_initialized)
        return ASIOTrue;

    m_sysHandle = reinterpret_cast<HWND>(sysHandle);

    // Try to open the WASAPI endpoints for the OMEC Teleport
    if (!m_usb->open())
    {
        std::strcpy(m_errorMsg, "Orange OMEC Teleport not found. Please connect the device.");
        // Non-fatal: host will call start() later; we return true so the driver loads.
    }
    else
    {
        m_usb->setGainProcessor(m_gain.get());
    }

    m_initialized = true;
    return ASIOTrue;
}

void OmecTeleportASIO::getDriverName(char* name)
{
    std::strcpy(name, "OmecTeleport ASIO");
}

long OmecTeleportASIO::getDriverVersion()
{
    return (OMEC_VERSION_MAJOR << 16) | (OMEC_VERSION_MINOR << 8) | OMEC_VERSION_PATCH;
}

void OmecTeleportASIO::getErrorMessage(char* string)
{
    std::strcpy(string, m_errorMsg);
}

// ============================================================================
// ASIOStart / Stop
// ============================================================================

ASIOError OmecTeleportASIO::start()
{
    if (m_streamRunning.load())
        return ASE_OK;
    if (!m_buffersCreated.load())
        return ASE_NotPresent;
    if (!m_usb->isOpen())
    {
        if (!m_usb->open())
        {
            std::strcpy(m_errorMsg, "Orange OMEC Teleport not found.");
            return ASE_HWMalfunction;
        }
        m_usb->setGainProcessor(m_gain.get());
    }

    const uint32_t sr = static_cast<uint32_t>(m_sampleRate.load());
    const uint32_t bs = static_cast<uint32_t>(m_bufferSize);

    // Set the ASIO callback — the audio thread calls this with exactly
    // bs frames, using a ring buffer to bridge any WASAPI/ASIO mismatch.
    m_usb->setAsioCallback(asioCallback, this, bs);
    m_usb->setResetRequestCallback(engineResetRequest, this);

    if (!m_usb->start(sr, bs))
    {
        std::strcpy(m_errorMsg, "Failed to start WASAPI streaming.");
        return ASE_HWMalfunction;
    }

    // The engine normally runs at the requested rate (AUTOCONVERTPCM); if it
    // had to fall back to the endpoint mix rate, adopt it and tell the host
    // rather than lying about the clock.
    const uint32_t actualSr = m_usb->deviceSampleRate();
    if (actualSr != sr && actualSr != 0)
    {
        OMEC_TRACEF("  Stream rate %u differs from requested %u — notifying host",
                    actualSr, sr);
        m_sampleRate.store(static_cast<double>(actualSr));
        if (m_callbacks && m_callbacks->sampleRateDidChange)
            m_callbacks->sampleRateDidChange(static_cast<ASIOSampleRate>(actualSr));
    }

    // Honest latency: WASAPI engine buffers + ring residency + ASIO buffer,
    // as computed by the engine — not just the ASIO double-buffer.
    const long inLat  = static_cast<long>(m_usb->inputLatencyFrames());
    const long outLat = static_cast<long>(m_usb->outputLatencyFrames());
    const double rateNow = m_sampleRate.load();
    OMEC_TRACEF("  ASIO bufferSize=%ld  latency in=%ld out=%ld frames @ %.0f Hz",
                m_bufferSize, inLat, outLat, rateNow);
    m_inputLatencySamples.store(inLat);
    m_outputLatencySamples.store(outLat);
    m_inputLatMs.store(static_cast<float>(inLat * 1000.0 / rateNow));
    m_outputLatMs.store(static_cast<float>(outLat * 1000.0 / rateNow));

    m_samplePos.store(0);
    m_streamRunning.store(true, std::memory_order_release);
    return ASE_OK;
}

ASIOError OmecTeleportASIO::stop()
{
    // No early-return on !m_streamRunning: the engine's fatal-error path
    // clears the flag itself before the host reacts to kAsioResetRequest,
    // and the engine must still be torn down here or its stream objects
    // leak on every reset cycle.  m_usb->stop() is idempotent.
    m_streamRunning.store(false, std::memory_order_release);
    m_usb->stop();
    // A calibration window can't complete without audio flowing — cancel it
    // so a stale window is never applied after a later restart.
    m_gain->cancelCalibration();
    return ASE_OK;
}

// ============================================================================
// Buffer management
// ============================================================================

ASIOError OmecTeleportASIO::getChannels(long* numIn, long* numOut)
{
    *numIn  = 2;
    *numOut = 2;
    return ASE_OK;
}

ASIOError OmecTeleportASIO::getLatencies(long* inputLatency, long* outputLatency)
{
    *inputLatency  = m_inputLatencySamples.load();
    *outputLatency = m_outputLatencySamples.load();
    if (*inputLatency == 0 || *outputLatency == 0)
    {
        // Not started yet — estimate honestly instead of quoting the bare
        // double-buffer: shared-mode WASAPI adds roughly a 10 ms period on
        // capture (engine buffer ~2 periods) and period + ring target on
        // render.
        const double sr = m_sampleRate.load();
        *inputLatency  = m_bufferSize + static_cast<long>(sr * 0.022);
        *outputLatency = m_bufferSize + static_cast<long>(sr * 0.032);
    }
    return ASE_OK;
}

ASIOError OmecTeleportASIO::getBufferSize(long* minSize, long* maxSize,
                                           long* preferredSize, long* granularity)
{
    *minSize     = 64;
    *maxSize     = 2048;
    *granularity = -1;   // powers of 2

    // Surface the user's saved preference (control panel, Advanced tab) so
    // the host actually opens the stream at the size the user chose.
    long pref = static_cast<long>(m_settings.bufferSize);
    const bool validPow2 = pref >= 64 && pref <= 2048 && (pref & (pref - 1)) == 0;
    *preferredSize = validPow2 ? pref : 128;
    return ASE_OK;
}

ASIOError OmecTeleportASIO::canSampleRate(ASIOSampleRate sampleRate)
{
    // In shared mode, WASAPI uses the system's configured sample rate.
    // Accept 44100 and 48000 — the Windows audio engine will resample if needed.
    if (sampleRate == 44100.0 || sampleRate == 48000.0)
        return ASE_OK;
    return ASE_NoClock;
}

ASIOError OmecTeleportASIO::getSampleRate(ASIOSampleRate* sampleRate)
{
    *sampleRate = m_sampleRate.load();
    return ASE_OK;
}

ASIOError OmecTeleportASIO::setSampleRate(ASIOSampleRate sampleRate)
{
    if (sampleRate != 44100.0 && sampleRate != 48000.0)
        return ASE_NoClock;

    const double oldRate = m_sampleRate.load();
    const bool changed   = (oldRate != sampleRate);
    m_sampleRate.store(sampleRate);

    if (changed && m_streamRunning.load())
    {
        // Restart streaming at the new rate — and actually check the result.
        stop();
        if (start() != ASE_OK)
        {
            // Could not run at the new rate: best-effort restore of the old
            // one, and tell the host the new rate was not accepted.
            OMEC_TRACEF("setSampleRate: restart @%.0f failed — reverting to %.0f",
                        sampleRate, oldRate);
            m_sampleRate.store(oldRate);
            start();   // best effort; if this also fails the stream stays stopped
            return ASE_NoClock;
        }
    }

    // Persist only after the rate change actually took effect.
    m_settings.sampleRate = static_cast<uint32_t>(m_sampleRate.load());
    m_registry->save(m_settings);
    return ASE_OK;
}

ASIOError OmecTeleportASIO::getClockSources(ASIOClockSource* clocks, long* numSources)
{
    clocks[0].index       = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup   = -1;
    clocks[0].isCurrentSource   = ASIOTrue;
    std::strcpy(clocks[0].name, "Internal");
    *numSources = 1;
    return ASE_OK;
}

ASIOError OmecTeleportASIO::setClockSource(long /*reference*/)
{
    return ASE_OK;   // only one source
}

ASIOError OmecTeleportASIO::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp)
{
    const long long pos = m_samplePos.load();
    sPos->lo = static_cast<unsigned long>(pos & 0xFFFFFFFF);
    sPos->hi = static_cast<unsigned long>(pos >> 32);

    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    // Convert to nanoseconds (100-ns ASIOTimeStamp units = 1e-7 s intervals)
    // ASIOTimeStamp is in nanoseconds according to SDK
    const long long ns = static_cast<long long>(cnt.QuadPart * 1e9 / freq.QuadPart);
    tStamp->lo = static_cast<unsigned long>(ns & 0xFFFFFFFF);
    tStamp->hi = static_cast<unsigned long>(ns >> 32);
    return ASE_OK;
}

ASIOError OmecTeleportASIO::getChannelInfo(ASIOChannelInfo* info)
{
    if (info->channel < 0 || info->channel > 1)
        return ASE_InvalidParameter;

    info->isActive  = m_buffersCreated.load() ? ASIOTrue : ASIOFalse;
    info->channelGroup = 0;
    info->type      = ASIOSTFloat32LSB;  // float throughout — no int16 conversion

    if (info->isInput)
        std::strcpy(info->name, info->channel == 0 ? "Input L" : "Input R");
    else
        std::strcpy(info->name, info->channel == 0 ? "Output L" : "Output R");

    return ASE_OK;
}

ASIOError OmecTeleportASIO::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                          long bufferSize, ASIOCallbacks* callbacks)
{
    // numChannels is whatever subset of I/O channels the host activates
    // (asio.h: an arbitrary sum of inputs and outputs) — e.g. 1 mono input
    // + 2 outputs = 3.  We have 2 in + 2 out, so 1..4 are all serviceable.
    if (numChannels < 1 || numChannels > 4)
        return ASE_InvalidMode;
    // Enforce what getBufferSize advertises: 64..2048, powers of two.
    if (bufferSize < 64 || bufferSize > 2048 ||
        (bufferSize & (bufferSize - 1)) != 0)
        return ASE_InvalidMode;
    if (!callbacks)
        return ASE_InvalidParameter;

    disposeBuffers();

    m_bufferSize = bufferSize;
    m_bufferSizeAtomic.store(bufferSize);
    // Deliberately NOT persisted to m_settings.bufferSize: the host's
    // transient choice must not clobber the user's saved preference,
    // which getBufferSize() surfaces as preferredSize.
    m_callbacks = callbacks;

    // Set up buffer pointers for host.
    // bufferInfos may contain 2 or 4 channels (2 in + 2 out).
    for (long i = 0; i < numChannels; ++i)
    {
        ASIOBufferInfo& bi = bufferInfos[i];
        if (bi.channelNum < 0 || bi.channelNum > 1)
            return ASE_InvalidParameter;

        int ch = bi.channelNum;
        if (bi.isInput)
        {
            bi.buffers[0] = m_inputBuf[ch][0];
            bi.buffers[1] = m_inputBuf[ch][1];
        }
        else
        {
            bi.buffers[0] = m_outputBuf[ch][0];
            bi.buffers[1] = m_outputBuf[ch][1];
        }
    }

    m_buffersCreated.store(true, std::memory_order_release);
    return ASE_OK;
}

ASIOError OmecTeleportASIO::disposeBuffers()
{
    stop();
    m_callbacks = nullptr;
    m_buffersCreated.store(false, std::memory_order_release);
    std::memset(m_inputBuf,  0, sizeof(m_inputBuf));
    std::memset(m_outputBuf, 0, sizeof(m_outputBuf));
    return ASE_OK;
}

// ============================================================================
// Control panel
// ============================================================================

ASIOError OmecTeleportASIO::controlPanel()
{
    m_panel->show(m_sysHandle, m_gain.get(), m_registry.get(), &m_settings,
                  &m_streamRunning, &m_sampleRate, &m_bufferSizeAtomic,
                  &m_inputLatMs, &m_outputLatMs);
    return ASE_OK;
}

ASIOError OmecTeleportASIO::future(long selector, void* /*opt*/)
{
    // Report capabilities
    switch (selector)
    {
    case kAsioEnableTimeCodeRead:
    case kAsioDisableTimeCodeRead:
    case kAsioSetInputMonitor:
    case kAsioSupportsInputMonitor:
        return ASE_NotPresent;

    case kAsioCanInputGain:
    case kAsioCanInputMeter:
    case kAsioCanOutputGain:
    case kAsioCanOutputMeter:
        return ASE_SUCCESS;  // we support these via control panel

    default:
        return ASE_NotPresent;
    }
}

ASIOError OmecTeleportASIO::outputReady()
{
    // Host signals output buffers are ready — set event to allow OUT thread to proceed
    SetEvent(m_outputReadyEvent);
    return ASE_OK;
}

// ============================================================================
// Engine fatal-error path — called from the audio thread when the stream
// dies unrecoverably (device removal, engine fault).  Mark the stream
// stopped and ask the host to reset the driver; hosts handle
// kAsioResetRequest asynchronously (stop/dispose/re-init).
void OmecTeleportASIO::engineResetRequest(void* ctx)
{
    auto* self = static_cast<OmecTeleportASIO*>(ctx);
    self->m_streamRunning.store(false, std::memory_order_release);
    std::strcpy(self->m_errorMsg, "Audio stream lost (device removed?) — reset requested.");
    if (self->m_callbacks && self->m_callbacks->asioMessage)
        self->m_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
}

// ============================================================================
// ASIO Buffer Callback — called directly from the WASAPI audio thread.
// All data is float32 — no format conversion at the ASIO boundary.
void OmecTeleportASIO::asioCallback(void* ctx, const float* input,
                                     float* output, uint32_t frames)
{
    auto* self = static_cast<OmecTeleportASIO*>(ctx);
    if (!self->m_streamRunning.load(std::memory_order_acquire))
        return;

    const int next = self->m_activeBuffer.load(std::memory_order_acquire) ^ 1;

    // Deinterleave float capture → ASIO float input buffers
    for (uint32_t s = 0; s < frames; ++s)
    {
        self->m_inputBuf[0][next][s] = input[s * 2 + 0];
        self->m_inputBuf[1][next][s] = input[s * 2 + 1];
    }

    self->m_activeBuffer.store(next, std::memory_order_release);
    self->m_samplePos.fetch_add(frames);

    if (self->m_callbacks && self->m_callbacks->bufferSwitch)
        self->m_callbacks->bufferSwitch(next, ASIOTrue);

    // Interleave ASIO float output buffers → render
    for (uint32_t s = 0; s < frames; ++s)
    {
        output[s * 2 + 0] = self->m_outputBuf[0][next][s];
        output[s * 2 + 1] = self->m_outputBuf[1][next][s];
    }
}
