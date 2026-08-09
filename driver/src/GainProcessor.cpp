//-----------------------------------------------------------------------------
// GainProcessor.cpp
//-----------------------------------------------------------------------------

#include "GainProcessor.h"
#include <cstring>

GainProcessor::GainProcessor()
    : m_inputLinear_L(1.0f)
    , m_inputLinear_R(1.0f)
    , m_outputLinear(1.0f)
    , m_inputGainDB_L(0.0f)
    , m_inputGainDB_R(0.0f)
    , m_outputVolumeDB(0.0f)
    , m_inputPeak_L(0.0f)
    , m_inputPeak_R(0.0f)
    , m_outputPeak_L(0.0f)
    , m_outputPeak_R(0.0f)
    , m_softLimiter(true)
    , m_calibrating(false)
{
}

// ---- Hot path ---------------------------------------------------------------

void GainProcessor::processInput(float* buf, int numFrames) noexcept
{
    const float gainL = m_inputLinear_L.load(std::memory_order_relaxed);
    const float gainR = m_inputLinear_R.load(std::memory_order_relaxed);

    float peakL = m_inputPeak_L.load(std::memory_order_relaxed);
    float peakR = m_inputPeak_R.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i)
    {
        float sl = buf[i * 2 + 0] * gainL;
        buf[i * 2 + 0] = sl;
        float absL = std::abs(sl);
        if (absL > peakL) peakL = absL;

        float sr = buf[i * 2 + 1] * gainR;
        buf[i * 2 + 1] = sr;
        float absR = std::abs(sr);
        if (absR > peakR) peakR = absR;
    }

    m_inputPeak_L.store(peakL, std::memory_order_release);
    m_inputPeak_R.store(peakR, std::memory_order_release);
}

void GainProcessor::processOutput(float* buf, int numFrames) noexcept
{
    const float gain = m_outputLinear.load(std::memory_order_relaxed);

    float peakL = m_outputPeak_L.load(std::memory_order_relaxed);
    float peakR = m_outputPeak_R.load(std::memory_order_relaxed);

    for (int i = 0; i < numFrames; ++i)
    {
        // Scrub NaN/Inf from the host's plugin chain unconditionally —
        // this stage always runs, so bad samples never reach the shared
        // Windows audio engine even with the soft limiter disabled.
        float sl = buf[i * 2 + 0] * gain;
        if (!std::isfinite(sl)) sl = 0.0f;
        buf[i * 2 + 0] = sl;
        float absL = std::abs(sl);
        if (absL > peakL) peakL = absL;

        float sr = buf[i * 2 + 1] * gain;
        if (!std::isfinite(sr)) sr = 0.0f;
        buf[i * 2 + 1] = sr;
        float absR = std::abs(sr);
        if (absR > peakR) peakR = absR;
    }

    m_outputPeak_L.store(peakL, std::memory_order_release);
    m_outputPeak_R.store(peakR, std::memory_order_release);
}

// ---- Soft limiter -----------------------------------------------------------

void GainProcessor::applySoftLimiter(float* buf, int numFrames) noexcept
{
    if (!m_softLimiter.load(std::memory_order_relaxed))
        return;

    // Continuous soft knee engaging at -1 dBFS.  Below T the signal passes
    // untouched; above T it is mapped into [T, 1.0) with
    //     y = T + (1-T) * tanh((|s| - T) / (1-T))
    // which is C1-continuous at the threshold (value T, slope 1) and
    // asymptotes at full scale — no waveform step at the crossing, unlike
    // the previous piecewise form which jumped by ~-2.4 dB at |s| = T.
    static constexpr float T        = 0.891f;          // 10^(-1/20)
    static constexpr float range    = 1.0f - T;
    static constexpr float invRange = 1.0f / range;

    for (int i = 0; i < numFrames * 2; ++i)
    {
        float s = buf[i];
        if (!std::isfinite(s)) { buf[i] = 0.0f; continue; }  // scrub NaN/Inf
        const float a = std::abs(s);
        if (a > T)
        {
            const float y = T + range * std::tanh((a - T) * invRange);
            buf[i] = (s < 0.0f) ? -y : y;
        }
    }
}

// ---- Gain control -----------------------------------------------------------

void GainProcessor::setInputGainDB_L(float db) noexcept
{
    db = clampDB(db, -60.0f, 12.0f);
    m_inputGainDB_L.store(db, std::memory_order_release);
    m_inputLinear_L.store(dbToLinear(db), std::memory_order_release);
}

void GainProcessor::setInputGainDB_R(float db) noexcept
{
    db = clampDB(db, -60.0f, 12.0f);
    m_inputGainDB_R.store(db, std::memory_order_release);
    m_inputLinear_R.store(dbToLinear(db), std::memory_order_release);
}

void GainProcessor::setInputGainDB(float db) noexcept
{
    setInputGainDB_L(db);
    setInputGainDB_R(db);
}

void GainProcessor::setOutputVolumeDB(float db) noexcept
{
    db = clampDB(db, -60.0f, 0.0f);
    m_outputVolumeDB.store(db, std::memory_order_release);
    m_outputLinear.store(dbToLinear(db), std::memory_order_release);
}

// ---- Peak metering ----------------------------------------------------------

void GainProcessor::decayPeaks(float decayFactor) noexcept
{
    // Multiply all peak values by decayFactor (should be < 1.0, e.g. 0.85)
    auto decay = [&](std::atomic<float>& p) {
        float v = p.load(std::memory_order_relaxed) * decayFactor;
        p.store(v, std::memory_order_relaxed);
    };
    decay(m_inputPeak_L);
    decay(m_inputPeak_R);
    decay(m_outputPeak_L);
    decay(m_outputPeak_R);
}

// ---- Calibration ------------------------------------------------------------
//
// Lock-free redesign: the audio thread feeds the (incremental) true-peak
// detector directly — no mutex, no sample buffer, no allocation on the
// Pro Audio thread.  The detector and frame counters are owned exclusively
// by the audio thread; the UI thread only sees the atomic result, published
// with release/acquire ordering via m_calibFinished.

void GainProcessor::beginCalibration(float targetPeakDBFS, float durationSeconds) noexcept
{
    m_targetPeakDBFS = std::clamp(targetPeakDBFS, -18.0f, -6.0f);
    m_calibDurationSec.store(std::clamp(durationSeconds, 3.0f, 15.0f),
                             std::memory_order_relaxed);
    m_calibFinished.store(false, std::memory_order_relaxed);
    // The audio thread resets its detector state when it sees the pending
    // flag — the UI thread never touches audio-owned state.
    m_calibPending.store(true, std::memory_order_relaxed);
    m_calibrating.store(true, std::memory_order_release);
}

bool GainProcessor::feedCalibration(const float* stereoSamples, int numFrames,
                                    uint32_t sampleRate) noexcept
{
    if (!m_calibrating.load(std::memory_order_acquire))
        return false;

    if (m_calibPending.exchange(false, std::memory_order_acq_rel))
    {
        m_peakDetector.reset();
        m_calibFramesFed = 0;
        const float dur = m_calibDurationSec.load(std::memory_order_relaxed);
        m_calibFramesTarget = static_cast<uint64_t>(
            dur * static_cast<float>(sampleRate > 0 ? sampleRate : 48000));
    }

    // Incremental 4x-oversampled true peak — bounded math per sample,
    // no locks, no allocation.
    m_peakDetector.feedStereoFloat(stereoSamples, static_cast<size_t>(numFrames));
    m_calibFramesFed += static_cast<uint64_t>(numFrames);

    if (m_calibFramesFed >= m_calibFramesTarget)
    {
        m_calibPeak.store(m_peakDetector.truePeakLinear(), std::memory_order_relaxed);
        m_calibFinished.store(true, std::memory_order_release);
        m_calibrating.store(false, std::memory_order_release);
        return false;   // signal "done" to caller
    }
    return true;
}

void GainProcessor::cancelCalibration() noexcept
{
    m_calibrating.store(false, std::memory_order_release);
    m_calibFinished.store(false, std::memory_order_release);
}

CalibrationResult GainProcessor::finishCalibration() noexcept
{
    CalibrationResult res;
    res.targetPeakDB = m_targetPeakDBFS;

    if (!m_calibFinished.exchange(false, std::memory_order_acq_rel))
    {
        res.success = false;
        return res;
    }

    const float peak = m_calibPeak.load(std::memory_order_relaxed);
    if (peak < 0.001f)
    {
        // No signal detected
        res.success = false;
        return res;
    }

    const float peakDB     = 20.0f * std::log10(peak);
    const float requiredDB = std::clamp(m_targetPeakDBFS - peakDB, -60.0f, 12.0f);

    res.success        = true;
    res.measuredPeakDB = peakDB;
    res.appliedGainDB  = requiredDB;

    // Apply the new gain immediately (both channels)
    setInputGainDB(requiredDB);

    return res;
}
