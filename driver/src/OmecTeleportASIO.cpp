//-----------------------------------------------------------------------------
// OmecTeleportASIO.cpp — Main ASIO driver implementation
//-----------------------------------------------------------------------------

#include "OmecTeleportASIO.h"
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
    return 0x00010000;  // v1.0
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

    if (!m_usb->start(sr, bs))
    {
        std::strcpy(m_errorMsg, "Failed to start WASAPI streaming.");
        return ASE_HWMalfunction;
    }

    const long actual = static_cast<long>(m_usb->actualFrames());
    OMEC_TRACEF("  ASIO bufferSize=%ld  WASAPI actual=%ld", m_bufferSize, actual);

    // Compute latency using WASAPI period (real hardware latency)
    m_inputLatencySamples.store(actual + m_bufferSize);
    m_outputLatencySamples.store(actual + m_bufferSize);
    m_inputLatMs.store(static_cast<float>((actual + m_bufferSize) * 1000.0 / sr));
    m_outputLatMs.store(static_cast<float>((actual + m_bufferSize) * 1000.0 / sr));

    m_samplePos.store(0);
    m_streamRunning.store(true, std::memory_order_release);
    return ASE_OK;
}

ASIOError OmecTeleportASIO::stop()
{
    if (!m_streamRunning.load())
        return ASE_OK;

    m_streamRunning.store(false, std::memory_order_release);
    m_usb->stop();
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
    if (*inputLatency == 0)
        *inputLatency = *outputLatency = m_bufferSize * 2;
    return ASE_OK;
}

ASIOError OmecTeleportASIO::getBufferSize(long* minSize, long* maxSize,
                                           long* preferredSize, long* granularity)
{
    *minSize       = 64;
    *maxSize       = 2048;
    *preferredSize = 128;
    *granularity   = -1;   // powers of 2
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

    const bool changed = (m_sampleRate.load() != sampleRate);
    m_sampleRate.store(sampleRate);
    m_settings.sampleRate = static_cast<uint32_t>(sampleRate);
    m_registry->save(m_settings);

    if (changed && m_streamRunning.load())
    {
        // Restart streaming at new rate
        stop();
        start();
    }
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
    if (numChannels != 4 && numChannels != 2)
        return ASE_InvalidParameter;
    if (bufferSize < 32 || bufferSize > 2048)
        return ASE_InvalidParameter;
    if (!callbacks)
        return ASE_InvalidParameter;

    disposeBuffers();

    m_bufferSize = bufferSize;
    m_bufferSizeAtomic.store(bufferSize);
    m_settings.bufferSize = static_cast<uint32_t>(bufferSize);
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
