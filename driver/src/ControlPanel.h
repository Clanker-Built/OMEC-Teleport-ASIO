#pragma once
//-----------------------------------------------------------------------------
// ControlPanel.h — Win32 tabbed ASIO control panel UI
//
// Embedded in the driver DLL.  ASIOControlPanel() spawns a dedicated UI thread
// that owns the message pump and dialog.  Multiple calls to Show() are
// idempotent — brings the existing window to the foreground.
//-----------------------------------------------------------------------------

#include <windows.h>
#include <atomic>

// Forward declarations
class GainProcessor;
class RegistrySettings;
struct DriverSettings;

class ControlPanel
{
public:
    ControlPanel();
    ~ControlPanel();

    // Show the control panel (non-blocking, creates UI thread if needed).
    // sysHandle: the HWND passed to ASIOInit (parent hint).
    void show(HWND parentHint, GainProcessor* gain, RegistrySettings* registry,
              DriverSettings* settings, std::atomic<bool>* streamRunning,
              std::atomic<double>* sampleRate, std::atomic<long>* bufferSize,
              std::atomic<float>* inputLatencyMs, std::atomic<float>* outputLatencyMs);

    // Hide / destroy the panel.
    void hide();

    bool isVisible() const noexcept;

    // DLL instance — must be set before show() is called
    static HINSTANCE s_hInstance;

private:

    // Dialog procs
    static INT_PTR CALLBACK MainDlgProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK TabStatusProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK TabInputProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK TabOutputProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK TabAdvancedProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK CalibrationProc(HWND, UINT, WPARAM, LPARAM);

    // Helpers
    void createTabs(HWND hTab);
    void switchTab(int index);
    static void applyDarkTheme(HWND hDlg);
    static void paintMeter(HWND hCtrl, float level);  // owner-draw peak meter
    void refreshStatus();
    void refreshInputTab();
    void refreshOutputTab();
    void startAutoSet();
    void onAutoSetTimer();

    // Window handles
    HWND m_hMain       = nullptr;
    HWND m_hTabs[4]    = {};    // Status, Input, Output, Advanced
    int  m_currentTab  = 0;

    // Shared state pointers (owned by OmecTeleportASIO)
    GainProcessor*       m_gain          = nullptr;
    RegistrySettings*    m_registry      = nullptr;
    DriverSettings*      m_settings      = nullptr;
    std::atomic<bool>*   m_streamRunning = nullptr;
    std::atomic<double>* m_sampleRate    = nullptr;
    std::atomic<long>*   m_bufferSize    = nullptr;
    std::atomic<float>*  m_inputLatMs    = nullptr;
    std::atomic<float>*  m_outputLatMs   = nullptr;

    // Modal dialog state
    std::atomic<bool>    m_uiRunning     { false };

    // Calibration state (UI thread only)
    bool                 m_calibActive   = false;
    int                  m_calibSecondsRemaining = 0;
    int                  m_calibTickCount = 0;
    UINT_PTR             m_timerID       = 0;
    float                m_calibTargetDB = -12.0f;

    // Peak hold for meters (UI thread only)
    float m_inputPeakHold_L  = 0.0f;
    float m_inputPeakHold_R  = 0.0f;
    float m_outputPeakHold_L = 0.0f;
    float m_outputPeakHold_R = 0.0f;
};
