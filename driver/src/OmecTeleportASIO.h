#pragma once
//-----------------------------------------------------------------------------
// OmecTeleportASIO.h — Main ASIO driver class
//
// Derives from IASIO and CUnknown (Steinberg SDK pattern).
// Owns the UsbEngine, GainProcessor, RegistrySettings, and ControlPanel.
//-----------------------------------------------------------------------------

// Must include COM headers BEFORE combase.h / Steinberg SDK headers.
// Do NOT use WIN32_LEAN_AND_MEAN here — it strips objbase.h and breaks DECLARE_INTERFACE.
#include <windows.h>
#include <objbase.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <memory>

// Steinberg ASIO SDK
#include "combase.h"
#include "iasiodrv.h"

// Our components
#include "WasapiEngine.h"
#include "GainProcessor.h"
#include "RegistrySettings.h"
#include "ControlPanel.h"

class OmecTeleportASIO : public IASIO, public CUnknown
{
public:
    OmecTeleportASIO(LPUNKNOWN pUnk, HRESULT* phr);
    ~OmecTeleportASIO() override;

    // COM boilerplate
    DECLARE_IUNKNOWN
    static CUnknown* CreateInstance(LPUNKNOWN pUnk, HRESULT* phr);
    HRESULT STDMETHODCALLTYPE NonDelegatingQueryInterface(REFIID riid, void** ppvObject) override;

    // ASIO interface
    ASIOBool init(void* sysHandle) override;
    void     getDriverName(char* name) override;
    long     getDriverVersion() override;
    void     getErrorMessage(char* string) override;

    ASIOError start() override;
    ASIOError stop() override;

    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize,
                            long* preferredSize, long* granularity) override;

    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;

    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;

    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;

    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                            long bufferSize, ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;

    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

private:
    // Called from WASAPI audio thread via callback — does bufferSwitch
    static void asioCallback(void* ctx, const float* input,
                             float* output, uint32_t frames);

    // Subsystems
    std::unique_ptr<WasapiEngine>    m_usb;
    std::unique_ptr<GainProcessor>   m_gain;
    std::unique_ptr<RegistrySettings> m_registry;
    std::unique_ptr<ControlPanel>    m_panel;

    DriverSettings m_settings;

    // ASIO state
    ASIOCallbacks*    m_callbacks        = nullptr;
    long              m_bufferSize       = 128;
    std::atomic<double> m_sampleRate     { 44100.0 };
    std::atomic<bool> m_buffersCreated   { false };
    std::atomic<bool> m_streamRunning    { false };
    std::atomic<long> m_bufferSizeAtomic { 128 };

    // Double buffers: [channel 0=L, 1=R][side 0/1][sample]
    static constexpr int MAX_BUF = 2048;
    float m_inputBuf[2][2][MAX_BUF]  = {};
    float m_outputBuf[2][2][MAX_BUF] = {};
    std::atomic<int>  m_activeBuffer    { 0 };

    HANDLE            m_outputReadyEvent = nullptr;

    // Sample position tracking
    std::atomic<long long> m_samplePos  { 0 };

    // Latency reporting (in samples)
    std::atomic<long> m_inputLatencySamples  { 0 };
    std::atomic<long> m_outputLatencySamples { 0 };

    // Latency in ms for UI display
    std::atomic<float> m_inputLatMs  { 0.0f };
    std::atomic<float> m_outputLatMs { 0.0f };

    // Error state
    char m_errorMsg[128] = {};

    bool m_initialized = false;
    HWND m_sysHandle   = nullptr;
};
