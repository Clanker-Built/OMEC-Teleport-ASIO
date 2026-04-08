#pragma once
//-----------------------------------------------------------------------------
// WasapiEngine.h -- WASAPI Shared Mode audio backend (float32 throughout)
//-----------------------------------------------------------------------------
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <thread>
#include <cstdint>

class GainProcessor;

// Callback: driver fills ASIO buffers and calls bufferSwitch inside this.
// input/output are interleaved float stereo (frames samples per channel).
using AsioProcessFunc = void(*)(void* ctx, const float* input,
                                 float* output, uint32_t frames);

// Lock-free SPSC ring buffer for interleaved float stereo samples.
class StereoRing
{
public:
    static constexpr uint32_t CAPACITY = 16384; // float samples (8192 stereo frames)
    static constexpr uint32_t MASK     = CAPACITY - 1;

    uint32_t available() const { return m_wr.load(std::memory_order_acquire)
                                      - m_rd.load(std::memory_order_acquire); }
    uint32_t freeSpace() const { return CAPACITY - available(); }

    void write(const float* data, uint32_t count)
    {
        uint32_t wr = m_wr.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; ++i)
            m_buf[(wr + i) & MASK] = data[i];
        m_wr.store(wr + count, std::memory_order_release);
    }

    void read(float* data, uint32_t count)
    {
        uint32_t rd = m_rd.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < count; ++i)
            data[i] = m_buf[(rd + i) & MASK];
        m_rd.store(rd + count, std::memory_order_release);
    }

    void clear() { m_wr.store(0); m_rd.store(0); }

private:
    float m_buf[CAPACITY]{};
    std::atomic<uint32_t> m_wr{0};
    std::atomic<uint32_t> m_rd{0};
};

class WasapiEngine
{
public:
    static constexpr size_t MAX_BUF = 2048;

    WasapiEngine();
    ~WasapiEngine();

    bool open();
    bool start(uint32_t sampleRate, uint32_t bufferSize);
    void stop();
    void close();

    bool     isOpen()            const { return m_open.load(); }
    uint32_t actualFrames()      const { return m_asioBufSize; }
    uint32_t minDeviceFrames()   const { return 64; }
    uint32_t deviceSampleRate()  const { return m_deviceSampleRate; }

    void setGainProcessor(GainProcessor* gp) { m_gain = gp; }
    void setBufferSize(uint32_t) {}

    void setAsioCallback(AsioProcessFunc func, void* ctx, uint32_t asioBufferSize)
    {
        m_asioFunc = func;
        m_asioCtx  = ctx;
        m_asioBufSize = asioBufferSize;
    }

    size_t inPipelineSamples()  const { return m_asioBufSize; }
    size_t outPipelineSamples() const { return m_asioBufSize; }

private:
    void audioThreadProc();
    bool findEndpoints(IMMDevice** ppCap, IMMDevice** ppRen);

    // WASAPI objects
    IMMDeviceEnumerator* m_enum   = nullptr;
    IMMDevice*           m_capDev = nullptr;
    IMMDevice*           m_renDev = nullptr;
    IAudioClient*        m_capAC  = nullptr;
    IAudioClient*        m_renAC  = nullptr;
    IAudioCaptureClient* m_capIC  = nullptr;
    IAudioRenderClient*  m_renIC  = nullptr;

    HANDLE m_capEvent = nullptr;
    HANDLE m_stopEvent = nullptr;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_open   {false};

    uint32_t m_deviceSampleRate = 48000;
    uint32_t m_asioBufSize      = 128;
    uint32_t m_capBufFrames     = 0;
    uint32_t m_renBufFrames     = 0;

    // Mix format info
    WORD m_capBps      = 32;
    WORD m_renBps      = 32;
    bool m_capIsFloat  = true;
    bool m_renIsFloat  = true;
    WORD m_capChannels = 2;
    WORD m_renChannels = 2;

    // Ring buffers: float interleaved stereo
    StereoRing m_capRing;
    StereoRing m_outRing;

    // Temp buffers for format conversion (audio thread only)
    float m_tmpF32[MAX_BUF * 2]{};

    AsioProcessFunc m_asioFunc = nullptr;
    void*           m_asioCtx  = nullptr;

    GainProcessor* m_gain = nullptr;

    // Drift diagnostics
    uint32_t m_driftTrimCount = 0;
};
