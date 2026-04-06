#pragma once
//-----------------------------------------------------------------------------
// GainProcessor.h — Digital gain application and peak metering
//
// Hot path (USB IN/OUT threads):
//   processInput()  — apply inputGain, update input peak meter
//   processOutput() — apply outputVolume, update output peak meter
//
// UI path (Control Panel thread):
//   setInputGainDB(), setOutputVolumeDB() — atomic updates
//   getInputPeakLinear() — atomic reads for meter display
//
// Calibration path (UI thread initiates, USB IN thread feeds samples):
//   beginCalibration()   — start collecting samples
//   isCalibrationActive()
//   cancelCalibration()
//   The USB IN thread checks isCalibrationActive() and calls feedCalibration().
//   After the window elapses, finishCalibration() computes the new gain.
//-----------------------------------------------------------------------------

#include <atomic>
#include <mutex>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

#include "TruePeakDetector.h"

struct CalibrationResult
{
    bool  success        = false;
    float measuredPeakDB = 0.0f;   // what we measured
    float appliedGainDB  = 0.0f;   // what we set the gain to
    float targetPeakDB   = -12.0f;
};

class GainProcessor
{
public:
    GainProcessor();
    ~GainProcessor() = default;

    // ---- Hot path (called from USB threads) --------------------------------

    // Process interleaved stereo float buffer in-place.
    // numFrames = number of stereo frames (total samples = numFrames * 2).
    void processInput(float* buf, int numFrames) noexcept;
    void processOutput(float* buf, int numFrames) noexcept;

    // ---- Gain control (UI thread) ------------------------------------------

    // Input gain: –60 to +12 dB.  If linked, setting L also sets R.
    void setInputGainDB_L(float db) noexcept;
    void setInputGainDB_R(float db) noexcept;
    void setInputGainDB(float db) noexcept;   // sets both

    float getInputGainDB_L() const noexcept { return m_inputGainDB_L.load(std::memory_order_acquire); }
    float getInputGainDB_R() const noexcept { return m_inputGainDB_R.load(std::memory_order_acquire); }

    // Output volume: –60 to 0 dB.  Always linked (mono pedal into stereo out).
    void  setOutputVolumeDB(float db) noexcept;
    float getOutputVolumeDB() const noexcept { return m_outputVolumeDB.load(std::memory_order_acquire); }

    // ---- Peak metering (UI thread reads) -----------------------------------

    float getInputPeakLinear_L() const noexcept  { return m_inputPeak_L.load(std::memory_order_acquire); }
    float getInputPeakLinear_R() const noexcept  { return m_inputPeak_R.load(std::memory_order_acquire); }
    float getOutputPeakLinear_L() const noexcept { return m_outputPeak_L.load(std::memory_order_acquire); }
    float getOutputPeakLinear_R() const noexcept { return m_outputPeak_R.load(std::memory_order_acquire); }

    // Call from UI timer to decay peak hold (call ~10×/sec)
    void decayPeaks(float decayFactor) noexcept;

    // ---- Calibration (UI thread controls, USB thread feeds) ----------------

    // Begin a calibration window.
    // targetPeakDBFS: desired level (e.g. –12.0f), duration: seconds
    void beginCalibration(float targetPeakDBFS, float durationSeconds) noexcept;
    bool isCalibrationActive() const noexcept { return m_calibrating.load(std::memory_order_acquire); }
    void cancelCalibration() noexcept;

    // Called from USB IN thread during calibration to accumulate samples.
    // Returns false once the calibration window has elapsed (caller should
    // then call finishCalibration() from the UI thread).
    bool feedCalibration(const float* stereoSamples, int numFrames, uint32_t sampleRate) noexcept;

    // Called from UI thread after feedCalibration returns false.
    CalibrationResult finishCalibration() noexcept;

private:
    static float dbToLinear(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
    static float linearToDb(float lin) noexcept
    {
        if (lin < 1e-6f) return -120.0f;
        return 20.0f * std::log10(lin);
    }
    static float clampDB(float db, float lo, float hi) noexcept
    {
        return std::clamp(db, lo, hi);
    }

    // Atomic gain values (linear) — read on hot path
    std::atomic<float> m_inputLinear_L;
    std::atomic<float> m_inputLinear_R;
    std::atomic<float> m_outputLinear;

    // Atomic dB values — read by UI
    std::atomic<float> m_inputGainDB_L;
    std::atomic<float> m_inputGainDB_R;
    std::atomic<float> m_outputVolumeDB;

    // Peak meters — written by USB threads, read by UI thread
    std::atomic<float> m_inputPeak_L;
    std::atomic<float> m_inputPeak_R;
    std::atomic<float> m_outputPeak_L;
    std::atomic<float> m_outputPeak_R;

    // Calibration state
    std::atomic<bool>  m_calibrating;
    float              m_targetPeakDBFS   = -12.0f;
    float              m_calibDuration    = 8.0f;
    float              m_calibElapsed     = 0.0f;   // in samples / (sr * 2 channels)
    uint32_t           m_calibSampleRate  = 44100;
    std::mutex         m_calibMutex;
    std::vector<float> m_calibSamples;               // accumulates stereo float pairs
    TruePeakDetector   m_peakDetector;
    bool               m_calibFinished    = false;
    CalibrationResult  m_calibResult;
};
