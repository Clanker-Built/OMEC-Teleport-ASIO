#pragma once
//-----------------------------------------------------------------------------
// TruePeakDetector.h — 4× oversampled true peak via cubic Hermite interpolation
//
// ITU-R BS.1770-4 simplified.  Used during the calibration window only.
// The full calibration buffer (≈350K samples for 8 sec at 44.1 kHz) is fed
// in batch after recording finishes — no real-time streaming constraints here.
//-----------------------------------------------------------------------------

#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

class TruePeakDetector
{
public:
    TruePeakDetector() noexcept { reset(); }

    void reset() noexcept
    {
        m_truePeak = 0.0f;
        m_history[0] = m_history[1] = m_history[2] = m_history[3] = 0.0f;
    }

    // Feed interleaved stereo int16 samples (L,R,L,R,...).
    // Uses max of L and R channels (guitar is mono-duplicated).
    void feedStereoInt16(const int16_t* samples, size_t frameCount) noexcept
    {
        for (size_t i = 0; i < frameCount; ++i)
        {
            float l = samples[i * 2 + 0] * (1.0f / 32768.0f);
            float r = samples[i * 2 + 1] * (1.0f / 32768.0f);
            float s = (std::abs(l) >= std::abs(r)) ? l : r;
            processSample(s);
        }
    }

    // Feed interleaved stereo float samples (L,R,L,R,...).
    void feedStereoFloat(const float* samples, size_t frameCount) noexcept
    {
        for (size_t i = 0; i < frameCount; ++i)
        {
            float l = samples[i * 2 + 0];
            float r = samples[i * 2 + 1];
            float s = (std::abs(l) >= std::abs(r)) ? l : r;
            processSample(s);
        }
    }

    // Feed mono float samples.
    void feedMono(const float* samples, size_t count) noexcept
    {
        for (size_t i = 0; i < count; ++i)
            processSample(samples[i]);
    }

    // True peak in linear scale [0, ~1.0].  May slightly exceed 1.0 for
    // inter-sample peaks.
    float truePeakLinear() const noexcept { return m_truePeak; }

    // True peak in dBFS.  Returns –120 dBFS if no signal.
    float truePeakDBFS() const noexcept
    {
        if (m_truePeak < 1e-6f)
            return -120.0f;
        return 20.0f * std::log10(m_truePeak);
    }

private:
    // Cubic Hermite interpolation at t in [0,1)
    // p0..p3 are four consecutive samples (p1 is the "current" sample).
    static float hermite(float p0, float p1, float p2, float p3, float t) noexcept
    {
        const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
        const float b =        p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
        const float c = -0.5f * p0             + 0.5f * p2;
        const float d =               p1;
        return ((a * t + b) * t + c) * t + d;
    }

    void processSample(float s) noexcept
    {
        // Shift history: history = [p_{n-1}, p_n, p_{n+1}, p_{n+2}]
        // We maintain a 4-tap window.  history[1] is the "current" sample.
        m_history[0] = m_history[1];
        m_history[1] = m_history[2];
        m_history[2] = m_history[3];
        m_history[3] = s;

        // Track original sample
        m_truePeak = std::max(m_truePeak, std::abs(m_history[1]));

        // 4× oversample: compute 3 interpolated points between history[1] and history[2]
        for (int os = 1; os < 4; ++os)
        {
            const float t = os * 0.25f;
            const float interp = hermite(m_history[0], m_history[1], m_history[2], m_history[3], t);
            m_truePeak = std::max(m_truePeak, std::abs(interp));
        }
    }

    float m_history[4];
    float m_truePeak;
};
