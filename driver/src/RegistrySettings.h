#pragma once
//-----------------------------------------------------------------------------
// RegistrySettings.h — Persistent driver settings via Windows registry
// Stored under HKCU\Software\OmecTeleportASIO (per-user, no admin needed).
//-----------------------------------------------------------------------------

#include <cstdint>
#include <windows.h>

struct DriverSettings
{
    float    inputGainDB_L       = -12.0f;  // –60 to +12 dB  (default: –12 dB safe start)
    float    inputGainDB_R       = -12.0f;
    float    outputVolumeDB      = 0.0f;    // –60 to 0 dB
    uint32_t sampleRate          = 44100;   // 44100 or 48000
    uint32_t bufferSize          = 128;     // ASIO buffer in samples
    float    targetPeakDBFS      = -12.0f;  // calibration target, –6 to –18
    float    calibrationDuration = 8.0f;    // seconds
    bool     linkInputChannels   = true;    // L/R gain linked in UI
    bool     softLimiterEnabled  = true;    // soft limiter at -1 dBFS on output
};

class RegistrySettings
{
public:
    static constexpr wchar_t KEY[] = L"Software\\OmecTeleportASIO";

    // Load settings; fills `out` with defaults if key absent.
    // Returns true if the key existed (even partially).
    bool load(DriverSettings& out);

    // Save all settings.  Returns true on success.
    bool save(const DriverSettings& in);

private:
    static void setFloat(HKEY hk, const wchar_t* name, float val);
    static void setDword(HKEY hk, const wchar_t* name, uint32_t val);
    static float getFloat(HKEY hk, const wchar_t* name, float def);
    static uint32_t getDword(HKEY hk, const wchar_t* name, uint32_t def);
};
