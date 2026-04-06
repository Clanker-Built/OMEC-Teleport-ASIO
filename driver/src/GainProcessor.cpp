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
        float sl = buf[i * 2 + 0] * gain;
        buf[i * 2 + 0] = sl;
        float absL = std::abs(sl);
        if (absL > peakL) peakL = absL;

        float sr = buf[i * 2 + 1] * gain;
        buf[i * 2 + 1] = sr;
        float absR = std::abs(sr);
        if (absR > peakR) peakR = absR;
    }

    m_outputPeak_L.store(peakL, std::memory_order_release);
    m_outputPeak_R.store(peakR, std::memory_order_release);
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

void GainProcessor::beginCalibration(float targetPeakDBFS, float durationSeconds) noexcept
{
    std::lock_guard<std::mutex> lock(m_calibMutex);
    m_targetPeakDBFS  = std::clamp(targetPeakDBFS, -18.0f, -6.0f);
    m_calibDuration   = std::clamp(durationSeconds, 3.0f, 15.0f);
    m_calibElapsed    = 0.0f;
    m_calibSamples.clear();
    m_calibSamples.reserve(static_cast<size_t>(m_calibDuration * 44100 * 2 * 1.1f));
    m_peakDetector.reset();
    m_calibFinished   = false;
    m_calibrating.store(true, std::memory_order_release);
}

bool GainProcessor::feedCalibration(const float* stereoSamples, int numFrames,
                                    uint32_t sampleRate) noexcept
{
    if (!m_calibrating.load(std::memory_order_acquire))
        return false;

    std::lock_guard<std::mutex> lock(m_calibMutex);
    if (!m_calibrating.load(std::memory_order_relaxed))
        return false;

    m_calibSampleRate = sampleRate;

    // Append raw samples before gain is applied (call this BEFORE processInput)
    m_calibSamples.insert(m_calibSamples.end(),
                          stereoSamples,
                          stereoSamples + numFrames * 2);

    m_calibElapsed += static_cast<float>(numFrames) / static_cast<float>(sampleRate);

    if (m_calibElapsed >= m_calibDuration)
    {
        m_calibrating.store(false, std::memory_order_release);
        m_calibFinished = true;
        return false;   // signal "done" to caller
    }
    return true;
}

void GainProcessor::cancelCalibration() noexcept
{
    m_calibrating.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_calibMutex);
    m_calibSamples.clear();
    m_calibFinished = false;
}

CalibrationResult GainProcessor::finishCalibration() noexcept
{
    std::lock_guard<std::mutex> lock(m_calibMutex);

    CalibrationResult res;
    res.targetPeakDB = m_targetPeakDBFS;

    if (!m_calibFinished || m_calibSamples.empty())
    {
        res.success = false;
        return res;
    }

    // Compute true peak over all accumulated samples
    m_peakDetector.reset();
    m_peakDetector.feedStereoFloat(m_calibSamples.data(),
                                    m_calibSamples.size() / 2);

    const float peak = m_peakDetector.truePeakLinear();
    if (peak < 0.001f)
    {
        // No signal detected
        res.success = false;
        m_calibSamples.clear();
        m_calibFinished = false;
        return res;
    }

    const float peakDB      = m_peakDetector.truePeakDBFS();
    const float requiredDB  = std::clamp(m_targetPeakDBFS - peakDB, -60.0f, 12.0f);

    res.success        = true;
    res.measuredPeakDB = peakDB;
    res.appliedGainDB  = requiredDB;

    // Apply the new gain immediately (both channels)
    setInputGainDB(requiredDB);

    m_calibResult   = res;
    m_calibSamples.clear();
    m_calibFinished = false;

    return res;
}
