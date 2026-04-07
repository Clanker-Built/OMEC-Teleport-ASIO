//-----------------------------------------------------------------------------
// RegistrySettings.cpp
//-----------------------------------------------------------------------------

#include "RegistrySettings.h"
#include <cstring>

static inline float dwordToFloat(uint32_t d) noexcept
{
    float f;
    std::memcpy(&f, &d, 4);
    return f;
}

static inline uint32_t floatToDword(float f) noexcept
{
    uint32_t d;
    std::memcpy(&d, &f, 4);
    return d;
}

void RegistrySettings::setFloat(HKEY hk, const wchar_t* name, float val)
{
    uint32_t d = floatToDword(val);
    RegSetValueExW(hk, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&d), 4);
}

void RegistrySettings::setDword(HKEY hk, const wchar_t* name, uint32_t val)
{
    RegSetValueExW(hk, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), 4);
}

float RegistrySettings::getFloat(HKEY hk, const wchar_t* name, float def)
{
    DWORD val = 0, size = 4, type = 0;
    if (RegQueryValueExW(hk, name, nullptr, &type, reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS
        && type == REG_DWORD)
    {
        return dwordToFloat(static_cast<uint32_t>(val));
    }
    return def;
}

uint32_t RegistrySettings::getDword(HKEY hk, const wchar_t* name, uint32_t def)
{
    DWORD val = 0, size = 4, type = 0;
    if (RegQueryValueExW(hk, name, nullptr, &type, reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS
        && type == REG_DWORD)
    {
        return static_cast<uint32_t>(val);
    }
    return def;
}

bool RegistrySettings::load(DriverSettings& out)
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, KEY, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return false;

    out.inputGainDB_L       = getFloat(hk,  L"inputGainDB_L",       -12.0f);
    out.inputGainDB_R       = getFloat(hk,  L"inputGainDB_R",       -12.0f);
    out.outputVolumeDB      = getFloat(hk,  L"outputVolumeDB",      0.0f);
    out.sampleRate          = getDword(hk,  L"sampleRate",          44100);
    out.bufferSize          = getDword(hk,  L"bufferSize",          128);
    out.targetPeakDBFS      = getFloat(hk,  L"targetPeakDBFS",      -12.0f);
    out.calibrationDuration = getFloat(hk,  L"calibrationDuration", 8.0f);
    out.linkInputChannels   = getDword(hk,  L"linkInputChannels",   1) != 0;
    out.softLimiterEnabled  = getDword(hk,  L"softLimiterEnabled",  1) != 0;

    // Clamp loaded values to safe ranges
    if (out.inputGainDB_L < -60.0f) out.inputGainDB_L = -60.0f;
    if (out.inputGainDB_L >  12.0f) out.inputGainDB_L =  12.0f;
    if (out.inputGainDB_R < -60.0f) out.inputGainDB_R = -60.0f;
    if (out.inputGainDB_R >  12.0f) out.inputGainDB_R =  12.0f;
    if (out.outputVolumeDB < -60.0f) out.outputVolumeDB = -60.0f;
    if (out.outputVolumeDB >   0.0f) out.outputVolumeDB =   0.0f;
    if (out.sampleRate != 44100 && out.sampleRate != 48000) out.sampleRate = 44100;
    if (out.bufferSize < 32 || out.bufferSize > 2048) out.bufferSize = 128;
    if (out.targetPeakDBFS < -18.0f) out.targetPeakDBFS = -18.0f;
    if (out.targetPeakDBFS >  -6.0f) out.targetPeakDBFS =  -6.0f;
    if (out.calibrationDuration < 3.0f)  out.calibrationDuration = 3.0f;
    if (out.calibrationDuration > 15.0f) out.calibrationDuration = 15.0f;

    RegCloseKey(hk);
    return true;
}

bool RegistrySettings::save(const DriverSettings& in)
{
    HKEY hk = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, &disp) != ERROR_SUCCESS)
        return false;

    setFloat(hk, L"inputGainDB_L",       in.inputGainDB_L);
    setFloat(hk, L"inputGainDB_R",       in.inputGainDB_R);
    setFloat(hk, L"outputVolumeDB",      in.outputVolumeDB);
    setDword(hk, L"sampleRate",          in.sampleRate);
    setDword(hk, L"bufferSize",          in.bufferSize);
    setFloat(hk, L"targetPeakDBFS",      in.targetPeakDBFS);
    setFloat(hk, L"calibrationDuration", in.calibrationDuration);
    setDword(hk, L"linkInputChannels",   in.linkInputChannels ? 1 : 0);
    setDword(hk, L"softLimiterEnabled",  in.softLimiterEnabled ? 1 : 0);

    RegCloseKey(hk);
    return true;
}
