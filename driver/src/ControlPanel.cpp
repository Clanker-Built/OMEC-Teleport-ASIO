//-----------------------------------------------------------------------------
// ControlPanel.cpp — Win32 ASIO Control Panel implementation
//-----------------------------------------------------------------------------

#include "ControlPanel.h"
#include "GainProcessor.h"
#include "RegistrySettings.h"
#include "resource.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <winhttp.h>
#include <shellapi.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winhttp.lib")   // delay-loaded — see DelayLoadDLLs in driver.vcxproj
#pragma comment(lib, "shell32.lib")

HINSTANCE ControlPanel::s_hInstance = nullptr;

// Dark theme colours
static const COLORREF COL_BG         = RGB(30, 30, 30);
static const COLORREF COL_BG_CTRL    = RGB(45, 45, 48);
static const COLORREF COL_TEXT       = RGB(220, 220, 220);
static const COLORREF COL_HIGHLIGHT  = RGB(0, 122, 204);
static const COLORREF COL_GREEN      = RGB(0, 200, 80);
static const COLORREF COL_YELLOW     = RGB(230, 200, 0);
static const COLORREF COL_RED        = RGB(220, 40, 40);

static HBRUSH g_hBrBg   = nullptr;
static HBRUSH g_hBrCtrl = nullptr;

// Gain slider range: –60 to +12 dB mapped to slider positions 0–720
static constexpr int SLIDER_MIN_DB = -60;
static constexpr int SLIDER_MAX_DB =  12;
static constexpr int SLIDER_RANGE  = 720;  // 0.1 dB steps

static int dbToSlider(float db)
{
    float clamped = std::clamp(db, (float)SLIDER_MIN_DB, (float)SLIDER_MAX_DB);
    return static_cast<int>((clamped - SLIDER_MIN_DB) * SLIDER_RANGE /
                             (SLIDER_MAX_DB - SLIDER_MIN_DB));
}

static float sliderToDb(int pos)
{
    return SLIDER_MIN_DB + pos * (float)(SLIDER_MAX_DB - SLIDER_MIN_DB) / SLIDER_RANGE;
}

// ============================================================================
// GitHub update check (background thread, no UI blocking)
//
// The worker never touches a window handle: it publishes its result in
// g_update and the 100 ms status-tab timer polls it, so a panel closed and
// reopened mid-check still shows the notification.  The DLL is pinned for
// the thread's lifetime (GetModuleHandleEx + FreeLibraryAndExitThread) so
// the host can never unload it out from under the thread.  kDone is latched
// only after a completed HTTP 200 exchange; failures return to kIdle so the
// next panel open retries.
// ============================================================================

namespace {

enum UpdateState : int { kIdle = 0, kRunning = 1, kDone = 2 };

struct UpdateInfo {
    std::atomic<int>  state{kIdle};
    // version/url are written by the single worker thread before the
    // release-store of `available`; the UI thread reads them only after an
    // acquire-load sees `available` == true.
    std::atomic<bool> available{false};
    wchar_t version[32] = {};
    wchar_t url[512]    = {};
};
UpdateInfo g_update;

} // namespace

static bool parseJsonString(const char* json, const char* key,
                            char* out, size_t outLen)
{
    if (outLen == 0) return false;
    char pattern[64];
    sprintf_s(pattern, "\"%s\"", key);
    const char* pos = strstr(json, pattern);
    if (!pos) return false;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == ':' ||
           *pos == '\n' || *pos == '\r') ++pos;
    if (*pos != '"') return false;
    ++pos;
    size_t i = 0;
    while (*pos && *pos != '"' && i < outLen - 1)
        out[i++] = *pos++;
    out[i] = '\0';
    return i > 0;
}

// Parse up to three numeric fields from a tag like "v1.5.1", "1.6", or
// "release-1.5.2" (leading non-digits are skipped).  Missing fields are 0.
static bool parseVersionTag(const char* tag, int& maj, int& min, int& pat)
{
    while (*tag && (*tag < '0' || *tag > '9')) ++tag;
    if (!*tag) return false;
    maj = min = pat = 0;
    return sscanf_s(tag, "%d.%d.%d", &maj, &min, &pat) >= 1;
}

static bool isNewerThanLocal(const char* remoteTag)
{
    int rMaj = 0, rMin = 0, rPat = 0;
    if (!parseVersionTag(remoteTag, rMaj, rMin, rPat)) return false;
    if (rMaj != OMEC_VERSION_MAJOR) return rMaj > OMEC_VERSION_MAJOR;
    if (rMin != OMEC_VERSION_MINOR) return rMin > OMEC_VERSION_MINOR;
    return rPat > OMEC_VERSION_PATCH;
}

enum class CheckResult { Failed, Completed };

static CheckResult doUpdateCheck()
{
    // Automatic proxy resolution (per-user / IE / WPAD-PAC aware); fall back
    // to the machine-wide WinHTTP proxy on systems without it.
    HINTERNET hSession = WinHttpOpen(
        L"OmecTeleportASIO/" OMEC_VERSION_TAG_W,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        hSession = WinHttpOpen(
            L"OmecTeleportASIO/" OMEC_VERSION_TAG_W,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return CheckResult::Failed;

    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 5000);

    HINTERNET hConn = WinHttpConnect(hSession, L"api.github.com",
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hReq = nullptr;
    if (hConn)
        hReq = WinHttpOpenRequest(
            hConn, L"GET",
            L"/repos/Clanker-Built/OMEC-Teleport-ASIO/releases/latest",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);

    bool ok = hReq &&
              WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hReq, nullptr);

    if (ok)
    {
        DWORD status = 0, sz = sizeof(status);
        ok = WinHttpQueryHeaders(hReq,
                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                 WINHTTP_HEADER_NAME_BY_INDEX,
                 &status, &sz, WINHTTP_NO_HEADER_INDEX)
             && status == 200;
    }

    std::string body;
    if (ok)
    {
        // Hard bounds: 15 s wall clock, 1 MB body.  Real payload is ~20 KB.
        const ULONGLONG deadline = GetTickCount64() + 15000;
        constexpr size_t kMaxBody = 1024 * 1024;
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail)) { ok = false; break; }
            if (avail == 0) break;   // end of response
            if (GetTickCount64() > deadline ||
                body.size() + avail > kMaxBody) { ok = false; break; }
            const size_t offset = body.size();
            body.resize(offset + avail);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hReq, &body[offset], avail, &bytesRead))
            { ok = false; break; }
            body.resize(offset + bytesRead);
            if (bytesRead == 0) break;
        }
    }

    if (hReq)  WinHttpCloseHandle(hReq);
    if (hConn) WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);

    if (!ok) return CheckResult::Failed;

    char tagName[32] = {}, htmlUrl[512] = {};
    if (parseJsonString(body.c_str(), "tag_name", tagName, sizeof(tagName)) &&
        parseJsonString(body.c_str(), "html_url", htmlUrl, sizeof(htmlUrl)) &&
        strncmp(htmlUrl, "https://github.com/", 19) == 0 &&   // only ever open GitHub
        strstr(htmlUrl, "/releases/") != nullptr &&           // ...release pages
        isNewerThanLocal(tagName))
    {
        MultiByteToWideChar(CP_UTF8, 0, tagName, -1, g_update.version, 32);
        MultiByteToWideChar(CP_UTF8, 0, htmlUrl, -1, g_update.url, 512);
        g_update.available.store(true, std::memory_order_release);
    }
    return CheckResult::Completed;
}

// winhttp.dll is delay-loaded; if the loader ever fails to bind it, the
// delay-load helper raises one of these SEH codes at the first WinHttp call.
// Treat that as a failed (retryable) check instead of crashing the host.
// The filter is narrow on purpose — unrelated faults still crash loudly.
static DWORD delayLoadFilter(DWORD code)
{
    constexpr DWORD kDelayLoadModNotFound  = 0xC06D007E;  // VcppException(ERROR_MOD_NOT_FOUND)
    constexpr DWORD kDelayLoadProcNotFound = 0xC06D007F;  // VcppException(ERROR_PROC_NOT_FOUND)
    return (code == kDelayLoadModNotFound || code == kDelayLoadProcNotFound)
        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// Separate frame with no unwindable C++ objects so __try is legal (C2712).
static CheckResult doUpdateCheckGuarded()
{
    __try
    {
        return doUpdateCheck();
    }
    __except (delayLoadFilter(GetExceptionCode()))
    {
        return CheckResult::Failed;
    }
}

static DWORD WINAPI updateCheckThreadProc(LPVOID param)
{
    HMODULE hSelf = static_cast<HMODULE>(param);
    CheckResult r = doUpdateCheckGuarded();
    g_update.state.store(r == CheckResult::Completed ? kDone : kIdle,
                         std::memory_order_release);
    // Releases the module pin taken in startUpdateCheckOnce() and exits
    // without ever returning into (possibly unloaded) module code.
    FreeLibraryAndExitThread(hSelf, 0);
    return 0;   // not reached
}

static void startUpdateCheckOnce()
{
    // Single atomic claim: only one thread can move Idle -> Running, so a
    // panel reopened while a check is in flight (or racing its completion)
    // can never spawn a duplicate worker.
    int expected = kIdle;
    if (!g_update.state.compare_exchange_strong(expected, kRunning,
                                                std::memory_order_acq_rel))
        return;   // already running, or completed for this DLL load

    // Pin the DLL so it cannot be unloaded while the worker runs.
    HMODULE hSelf = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&startUpdateCheckOnce),
                            &hSelf))
    {
        g_update.state.store(kIdle, std::memory_order_release);
        return;
    }

    HANDLE hThread = CreateThread(nullptr, 0, updateCheckThreadProc,
                                  hSelf, 0, nullptr);
    if (!hThread)
    {
        FreeLibrary(hSelf);
        g_update.state.store(kIdle, std::memory_order_release);
        return;
    }
    CloseHandle(hThread);
}

// ============================================================================
// ControlPanel lifecycle
// ============================================================================

ControlPanel::ControlPanel()
{
    // Initialise common controls (tab control, progress bar, trackbar)
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    if (!g_hBrBg)   g_hBrBg   = CreateSolidBrush(COL_BG);
    if (!g_hBrCtrl) g_hBrCtrl = CreateSolidBrush(COL_BG_CTRL);
}

ControlPanel::~ControlPanel()
{
    hide();
}

void ControlPanel::show(HWND parentHint, GainProcessor* gain, RegistrySettings* registry,
                        DriverSettings* settings, std::atomic<bool>* streamRunning,
                        std::atomic<double>* sampleRate, std::atomic<long>* bufferSize,
                        std::atomic<float>* inputLatMs, std::atomic<float>* outputLatMs)
{
    m_gain          = gain;
    m_registry      = registry;
    m_settings      = settings;
    m_streamRunning = streamRunning;
    m_sampleRate    = sampleRate;
    m_bufferSize    = bufferSize;
    m_inputLatMs    = inputLatMs;
    m_outputLatMs   = outputLatMs;

    if (m_uiRunning.load())
    {
        if (m_hMain) SetForegroundWindow(m_hMain);
        return;
    }

    // Self-discover our HINSTANCE (DllRegisterServer sets it, but normal
    // host loads do not).
    if (!s_hInstance)
    {
        static const char s_anchor = 0;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            &s_anchor,
            reinterpret_cast<HMODULE*>(&s_hInstance));
    }

    if (!s_hInstance) return;   // cannot load resources

    // Modal dialog on the caller's thread — the standard ASIO control panel
    // pattern (ASIO4ALL, RME, etc.).  No background thread, no cross-thread
    // parent issues, no deadlocks.
    m_uiRunning.store(true);
    DialogBoxParamW(s_hInstance, MAKEINTRESOURCEW(IDD_MAIN_DIALOG),
                    parentHint, MainDlgProc, reinterpret_cast<LPARAM>(this));
    m_hMain = nullptr;
    m_uiRunning.store(false);
}

void ControlPanel::hide()
{
    if (m_hMain)
    {
        EndDialog(m_hMain, 0);
        m_hMain = nullptr;
    }
    m_uiRunning.store(false);
}

bool ControlPanel::isVisible() const noexcept
{
    return m_uiRunning.load() && m_hMain && IsWindowVisible(m_hMain);
}

// ============================================================================
// Dark theme helpers
// ============================================================================

void ControlPanel::applyDarkTheme(HWND hDlg)
{
    SetWindowTheme(hDlg, L"DarkMode_Explorer", nullptr);
    InvalidateRect(hDlg, nullptr, TRUE);
}

// ============================================================================
// Peak meter owner-draw paint
// level: 0.0 to 1.0 (linear)
// ============================================================================
void ControlPanel::paintMeter(HWND hCtrl, float level)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hCtrl, &ps);

    RECT rc;
    GetClientRect(hCtrl, &rc);

    // Background
    HBRUSH hbBg = CreateSolidBrush(RGB(20, 20, 20));
    FillRect(hdc, &rc, hbBg);
    DeleteObject(hbBg);

    // Filled portion
    const float levelClamped = std::clamp(level, 0.0f, 1.0f);
    const int   filledWidth  = static_cast<int>(levelClamped * (rc.right - rc.left));

    if (filledWidth > 0)
    {
        // Convert linear to dBFS for colour selection
        float db = (level > 1e-6f) ? 20.0f * std::log10(level) : -120.0f;
        COLORREF col = (db > -3.0f) ? COL_RED : (db > -12.0f) ? COL_YELLOW : COL_GREEN;

        RECT fill = rc;
        fill.right = rc.left + filledWidth;
        HBRUSH hbFill = CreateSolidBrush(col);
        FillRect(hdc, &fill, hbFill);
        DeleteObject(hbFill);
    }

    EndPaint(hCtrl, &ps);
}

// ============================================================================
// Tab management
// ============================================================================

void ControlPanel::createTabs(HWND hTab)
{
    // ANSI strings — project uses MultiByte charset, so TabCtrl_InsertItem
    // resolves to the ANSI version. Wide strings would show only 1 char.
    static const char* tabNames[] = { "Device Status", "Input Controls",
                                       "Output Controls", "Advanced" };
    static const int tabDialogs[] = { IDD_TAB_STATUS, IDD_TAB_INPUT,
                                       IDD_TAB_OUTPUT, IDD_TAB_ADVANCED };
    static DLGPROC tabProcs[]     = { TabStatusProc, TabInputProc,
                                       TabOutputProc, TabAdvancedProc };

    // Inherit the dialog font so tab labels render at the correct size.
    HFONT hFont = reinterpret_cast<HFONT>(
        SendMessageW(GetParent(hTab), WM_GETFONT, 0, 0));
    if (hFont)
        SendMessageW(hTab, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

    // Strip visual styles — dark-mode themes collapse tab label widths.
    SetWindowTheme(hTab, L"", L"");

    // Compute fixed tab width from the tab control's actual pixel width.
    RECT rcTab;
    GetClientRect(hTab, &rcTab);
    int tabWidth = (rcTab.right - rcTab.left) / 4 - 3;
    SendMessageA(hTab, TCM_SETITEMSIZE, 0, MAKELPARAM(tabWidth, 22));

    TCITEMA tie = {};
    tie.mask    = TCIF_TEXT;
    for (int i = 0; i < 4; ++i)
    {
        tie.pszText = const_cast<char*>(tabNames[i]);
        SendMessageA(hTab, TCM_INSERTITEMA, i, reinterpret_cast<LPARAM>(&tie));
    }

    // Create all child dialogs, initially hidden
    RECT tabRect;
    GetClientRect(hTab, &tabRect);
    TabCtrl_AdjustRect(hTab, FALSE, &tabRect);

    HWND hParent = GetParent(hTab);
    for (int i = 0; i < 4; ++i)
    {
        m_hTabs[i] = CreateDialogParamW(s_hInstance,
                                         MAKEINTRESOURCEW(tabDialogs[i]),
                                         hParent,
                                         tabProcs[i],
                                         reinterpret_cast<LPARAM>(this));
        SetWindowPos(m_hTabs[i], HWND_TOP,
                     tabRect.left + 4, tabRect.top + 4,
                     tabRect.right - tabRect.left - 8,
                     tabRect.bottom - tabRect.top - 8,
                     i == 0 ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    }
}

void ControlPanel::switchTab(int index)
{
    for (int i = 0; i < 4; ++i)
        ShowWindow(m_hTabs[i], i == index ? SW_SHOW : SW_HIDE);
    m_currentTab = index;
}

// ============================================================================
// Refresh helpers (called from WM_TIMER)
// ============================================================================

void ControlPanel::refreshStatus()
{
    if (!m_hTabs[0] || !IsWindow(m_hTabs[0])) return;
    HWND hTab = m_hTabs[0];

    bool connected = m_streamRunning && m_streamRunning->load();
    SetDlgItemTextW(hTab, IDC_STATUS_DEVICE,
                    connected ? L"Orange OMEC Teleport (connected)" : L"Not connected");

    if (m_sampleRate)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%.0f Hz", m_sampleRate->load());
        SetDlgItemTextW(hTab, IDC_STATUS_SAMPLERATE, buf);
    }
    if (m_bufferSize)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%ld samples", m_bufferSize->load());
        SetDlgItemTextW(hTab, IDC_STATUS_BUFFERSIZE, buf);
    }
    if (m_inputLatMs)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%.1f ms", m_inputLatMs->load());
        SetDlgItemTextW(hTab, IDC_STATUS_LATENCY_IN, buf);
    }
    if (m_outputLatMs)
    {
        wchar_t buf[64];
        swprintf_s(buf, L"%.1f ms", m_outputLatMs->load());
        SetDlgItemTextW(hTab, IDC_STATUS_LATENCY_OUT, buf);
    }

    // Update-check result — polled here (not posted from the worker) so a
    // panel opened at any point after the check finishes still shows it.
    if (!m_updateShown && g_update.available.load(std::memory_order_acquire))
    {
        m_updateShown = true;
        wchar_t buf[64];
        swprintf_s(buf, L"Update available: %s", g_update.version);
        SetDlgItemTextW(hTab, IDC_STATUS_UPDATE, buf);
        swprintf_s(buf, L"Download %s", g_update.version);
        SetDlgItemTextW(hTab, IDC_STATUS_UPDATE_BTN, buf);
        ShowWindow(GetDlgItem(hTab, IDC_STATUS_UPDATE_BTN), SW_SHOW);
    }
}

void ControlPanel::refreshInputTab()
{
    if (!m_gain || !m_hTabs[1] || !IsWindow(m_hTabs[1])) return;
    HWND hTab = m_hTabs[1];

    // Update peak meters via InvalidateRect (owner-drawn)
    float peakL = m_gain->getInputPeakLinear_L();
    float peakR = m_gain->getInputPeakLinear_R();
    if (peakL > m_inputPeakHold_L) m_inputPeakHold_L = peakL;
    if (peakR > m_inputPeakHold_R) m_inputPeakHold_R = peakR;

    InvalidateRect(GetDlgItem(hTab, IDC_INPUT_METER_L), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hTab, IDC_INPUT_METER_R), nullptr, FALSE);

    // Decay peak hold
    m_inputPeakHold_L *= 0.90f;
    m_inputPeakHold_R *= 0.90f;
    m_gain->decayPeaks(0.95f);

    // Auto-set calibration polling
    if (m_calibActive && m_gain)
    {
        if (!m_gain->isCalibrationActive())
        {
            // Calibration window finished — compute gain
            CalibrationResult result = m_gain->finishCalibration();
            m_calibActive = false;

            wchar_t msg[256];
            if (result.success)
            {
                swprintf_s(msg,
                    L"Measured peak: %.1f dBFS → Applied %.1f dB → New target: %.1f dBFS",
                    result.measuredPeakDB, result.appliedGainDB, result.targetPeakDB);
                // Save to registry
                if (m_settings && m_registry)
                {
                    m_settings->inputGainDB_L = result.appliedGainDB;
                    m_settings->inputGainDB_R = result.appliedGainDB;
                    m_registry->save(*m_settings);
                }
            }
            else
            {
                wcscpy_s(msg, L"No signal detected. Please ensure the guitar is playing.");
            }
            SetDlgItemTextW(hTab, IDC_AUTOSET_STATUS, msg);
            EnableWindow(GetDlgItem(hTab, IDC_AUTOSET_BTN), TRUE);

            // Reset progress bar
            SendDlgItemMessageW(hTab, IDC_AUTOSET_PROGRESS, PBM_SETPOS, 0, 0);

            // Refresh sliders with new gain
            if (m_gain)
            {
                int posL = dbToSlider(m_gain->getInputGainDB_L());
                int posR = dbToSlider(m_gain->getInputGainDB_R());
                SendDlgItemMessageW(hTab, IDC_INPUT_GAIN_L, TBM_SETPOS, TRUE, posL);
                SendDlgItemMessageW(hTab, IDC_INPUT_GAIN_R, TBM_SETPOS, TRUE, posR);

                wchar_t buf[32];
                swprintf_s(buf, L"%.1f dB", m_gain->getInputGainDB_L());
                SetDlgItemTextW(hTab, IDC_INPUT_GAIN_L_VAL, buf);
                swprintf_s(buf, L"%.1f dB", m_gain->getInputGainDB_R());
                SetDlgItemTextW(hTab, IDC_INPUT_GAIN_R_VAL, buf);
            }
        }
        else
        {
            // Still calibrating — update progress bar.
            // Timer fires every 100ms; only decrement seconds every 10 ticks.
            ++m_calibTickCount;
            if (m_calibTickCount >= 10)
            {
                m_calibTickCount = 0;
                if (m_calibSecondsRemaining > 0)
                    --m_calibSecondsRemaining;
            }

            // The window only advances while audio is actually flowing.  If
            // the countdown has expired and the window still hasn't finished
            // (~3 s grace), the stream isn't running — bail out instead of
            // showing "0 seconds remaining" forever.
            if (m_calibSecondsRemaining == 0 && ++m_calibOvertimeTicks > 30)
            {
                if (m_gain) m_gain->cancelCalibration();
                m_calibActive = false;
                m_calibOvertimeTicks = 0;
                SetDlgItemTextW(hTab, IDC_AUTOSET_STATUS,
                    L"Calibration timed out — is audio running? Try again.");
                EnableWindow(GetDlgItem(hTab, IDC_AUTOSET_BTN), TRUE);
                SendDlgItemMessageW(hTab, IDC_AUTOSET_PROGRESS, PBM_SETPOS, 0, 0);
            }
            else if (m_settings)
            {
                int total = static_cast<int>(m_settings->calibrationDuration);
                int elapsed = total - m_calibSecondsRemaining;
                int pct = (total > 0) ? (elapsed * 100 / total) : 0;
                SendDlgItemMessageW(hTab, IDC_AUTOSET_PROGRESS, PBM_SETPOS, pct, 0);

                wchar_t buf[64];
                swprintf_s(buf, L"PLAY LOUD! %d seconds remaining...", m_calibSecondsRemaining);
                SetDlgItemTextW(hTab, IDC_AUTOSET_STATUS, buf);
            }
        }
    }
}

void ControlPanel::refreshOutputTab()
{
    if (!m_gain || !m_hTabs[2] || !IsWindow(m_hTabs[2])) return;
    HWND hTab = m_hTabs[2];

    float peakL = m_gain->getOutputPeakLinear_L();
    float peakR = m_gain->getOutputPeakLinear_R();
    if (peakL > m_outputPeakHold_L) m_outputPeakHold_L = peakL;
    if (peakR > m_outputPeakHold_R) m_outputPeakHold_R = peakR;

    InvalidateRect(GetDlgItem(hTab, IDC_OUTPUT_METER_L), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hTab, IDC_OUTPUT_METER_R), nullptr, FALSE);

    m_outputPeakHold_L *= 0.90f;
    m_outputPeakHold_R *= 0.90f;
}

// ============================================================================
// Main dialog proc
// ============================================================================

INT_PTR CALLBACK ControlPanel::MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ControlPanel* pThis = nullptr;
    if (msg == WM_INITDIALOG)
    {
        pThis = reinterpret_cast<ControlPanel*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hMain = hDlg;

        // Set minimum window size
        applyDarkTheme(hDlg);

        // Create tab control children
        HWND hTab = GetDlgItem(hDlg, IDC_TAB);
        pThis->createTabs(hTab);
        pThis->switchTab(0);

        // Start refresh timer (100 ms) — fires WM_TIMER in the modal loop
        pThis->m_timerID = SetTimer(hDlg, 1, 100, nullptr);

        // Kick off the background update check (at most once per DLL load;
        // failures are retried on the next panel open).  The result is
        // picked up by the timer poll in refreshStatus().
        pThis->m_updateShown = false;
        startUpdateCheckOnce();

        return TRUE;
    }

    pThis = reinterpret_cast<ControlPanel*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (!pThis) return FALSE;

    switch (msg)
    {
    case WM_TIMER:
        pThis->refreshStatus();
        pThis->refreshInputTab();
        pThis->refreshOutputTab();
        return TRUE;

    case WM_NOTIFY:
    {
        NMHDR* pnm = reinterpret_cast<NMHDR*>(lParam);
        if (pnm->idFrom == IDC_TAB && pnm->code == TCN_SELCHANGE)
        {
            int sel = TabCtrl_GetCurSel(GetDlgItem(hDlg, IDC_TAB));
            pThis->switchTab(sel);
        }
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR:
        SetTextColor(reinterpret_cast<HDC>(wParam), COL_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wParam), COL_BG);
        return reinterpret_cast<INT_PTR>(g_hBrBg);

    case WM_CLOSE:
        // Abandon any in-flight calibration so a stale window can never be
        // applied (and persisted) on a later panel open.
        if (pThis->m_calibActive)
        {
            if (pThis->m_gain) pThis->m_gain->cancelCalibration();
            pThis->m_calibActive = false;
        }
        KillTimer(hDlg, pThis->m_timerID);
        for (int i = 0; i < 4; ++i)
        {
            if (pThis->m_hTabs[i])
            {
                DestroyWindow(pThis->m_hTabs[i]);
                pThis->m_hTabs[i] = nullptr;
            }
        }
        EndDialog(hDlg, 0);    // closes the modal dialog — returns to show()
        return TRUE;
    }
    return FALSE;
}

// ============================================================================
// Input tab proc
// ============================================================================

INT_PTR CALLBACK ControlPanel::TabInputProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ControlPanel* pThis = nullptr;
    if (msg == WM_INITDIALOG)
    {
        pThis = reinterpret_cast<ControlPanel*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pThis));

        // Initialise sliders
        SendDlgItemMessageW(hDlg, IDC_INPUT_GAIN_L, TBM_SETRANGE, TRUE,
                            MAKELPARAM(0, SLIDER_RANGE));
        SendDlgItemMessageW(hDlg, IDC_INPUT_GAIN_R, TBM_SETRANGE, TRUE,
                            MAKELPARAM(0, SLIDER_RANGE));

        if (pThis->m_gain)
        {
            SendDlgItemMessageW(hDlg, IDC_INPUT_GAIN_L, TBM_SETPOS, TRUE,
                                dbToSlider(pThis->m_gain->getInputGainDB_L()));
            SendDlgItemMessageW(hDlg, IDC_INPUT_GAIN_R, TBM_SETPOS, TRUE,
                                dbToSlider(pThis->m_gain->getInputGainDB_R()));
        }

        if (pThis->m_settings)
            CheckDlgButton(hDlg, IDC_INPUT_LINK,
                           pThis->m_settings->linkInputChannels ? BST_CHECKED : BST_UNCHECKED);

        // Progress bar range 0–100
        SendDlgItemMessageW(hDlg, IDC_AUTOSET_PROGRESS, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        applyDarkTheme(hDlg);
        return TRUE;
    }

    pThis = reinterpret_cast<ControlPanel*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (!pThis) return FALSE;

    switch (msg)
    {
    case WM_HSCROLL:
    {
        bool linked = IsDlgButtonChecked(hDlg, IDC_INPUT_LINK) == BST_CHECKED;
        HWND hSlider = reinterpret_cast<HWND>(lParam);
        int  ctrl    = GetDlgCtrlID(hSlider);
        int  pos     = static_cast<int>(SendMessageW(hSlider, TBM_GETPOS, 0, 0));
        float db     = sliderToDb(pos);
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f dB", db);

        if (ctrl == IDC_INPUT_GAIN_L || (linked && ctrl == IDC_INPUT_GAIN_R))
        {
            if (linked)
            {
                pThis->m_gain->setInputGainDB(db);
                SendDlgItemMessageW(hDlg, IDC_INPUT_GAIN_L, TBM_SETPOS, TRUE, pos);
                SendDlgItemMessageW(hDlg, IDC_INPUT_GAIN_R, TBM_SETPOS, TRUE, pos);
                SetDlgItemTextW(hDlg, IDC_INPUT_GAIN_L_VAL, buf);
                SetDlgItemTextW(hDlg, IDC_INPUT_GAIN_R_VAL, buf);
            }
            else
            {
                pThis->m_gain->setInputGainDB_L(db);
                SetDlgItemTextW(hDlg, IDC_INPUT_GAIN_L_VAL, buf);
            }
        }
        else if (ctrl == IDC_INPUT_GAIN_R)
        {
            pThis->m_gain->setInputGainDB_R(db);
            SetDlgItemTextW(hDlg, IDC_INPUT_GAIN_R_VAL, buf);
        }

        // Persist
        if (pThis->m_settings && pThis->m_registry)
        {
            pThis->m_settings->inputGainDB_L = pThis->m_gain->getInputGainDB_L();
            pThis->m_settings->inputGainDB_R = pThis->m_gain->getInputGainDB_R();
            pThis->m_registry->save(*pThis->m_settings);
        }
        return TRUE;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_AUTOSET_BTN)
        {
            if (!pThis->m_calibActive && pThis->m_gain)
            {
                float targetDB = pThis->m_settings ? pThis->m_settings->targetPeakDBFS : -12.0f;
                float duration = pThis->m_settings ? pThis->m_settings->calibrationDuration : 8.0f;
                pThis->m_gain->beginCalibration(targetDB, duration);
                pThis->m_calibActive = true;
                pThis->m_calibSecondsRemaining = static_cast<int>(duration);
                pThis->m_calibTickCount = 0;
                pThis->m_calibOvertimeTicks = 0;
                EnableWindow(GetDlgItem(hDlg, IDC_AUTOSET_BTN), FALSE);
                SetDlgItemTextW(hDlg, IDC_AUTOSET_STATUS, L"PLAY YOUR LOUDEST NOW! Strum hard for the full duration.");
            }
        }
        else if (id == IDC_INPUT_LINK)
        {
            bool linked = IsDlgButtonChecked(hDlg, IDC_INPUT_LINK) == BST_CHECKED;
            if (pThis->m_settings)
            {
                pThis->m_settings->linkInputChannels = linked;
                if (pThis->m_registry) pThis->m_registry->save(*pThis->m_settings);
            }
        }
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* pDIS = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (pDIS->CtlID == IDC_INPUT_METER_L || pDIS->CtlID == IDC_INPUT_METER_R)
        {
            float level = (pDIS->CtlID == IDC_INPUT_METER_L)
                          ? pThis->m_inputPeakHold_L
                          : pThis->m_inputPeakHold_R;
            // Paint into the DRAWITEMSTRUCT DC
            RECT rc = pDIS->rcItem;
            rc.left = 0; rc.top = 0;
            rc.right  = pDIS->rcItem.right  - pDIS->rcItem.left;
            rc.bottom = pDIS->rcItem.bottom - pDIS->rcItem.top;

            HBRUSH hbBg = CreateSolidBrush(RGB(20, 20, 20));
            FillRect(pDIS->hDC, &rc, hbBg);
            DeleteObject(hbBg);

            float db = (level > 1e-6f) ? 20.0f * std::log10(level) : -120.0f;
            const int width = rc.right - rc.left;
            const int filled = static_cast<int>(std::clamp(
                (db + 60.0f) / 66.0f * width, 0.0f, (float)width));
            if (filled > 0)
            {
                COLORREF col = (db > -3.0f) ? COL_RED : (db > -12.0f) ? COL_YELLOW : COL_GREEN;
                RECT fill = { 0, 0, filled, rc.bottom };
                HBRUSH hb = CreateSolidBrush(col);
                FillRect(pDIS->hDC, &fill, hb);
                DeleteObject(hb);
            }
        }
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), COL_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wParam), COL_BG);
        return reinterpret_cast<INT_PTR>(g_hBrBg);
    }
    return FALSE;
}

// ============================================================================
// Output tab proc
// ============================================================================

INT_PTR CALLBACK ControlPanel::TabOutputProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ControlPanel* pThis = nullptr;
    if (msg == WM_INITDIALOG)
    {
        pThis = reinterpret_cast<ControlPanel*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pThis));

        // Volume slider: –60 to 0 dB, 600 steps (0.1 dB each)
        SendDlgItemMessageW(hDlg, IDC_OUTPUT_VOL, TBM_SETRANGE, TRUE, MAKELPARAM(0, 600));
        if (pThis->m_gain)
        {
            float db = pThis->m_gain->getOutputVolumeDB();
            int pos  = static_cast<int>((db + 60.0f) * 10.0f);
            SendDlgItemMessageW(hDlg, IDC_OUTPUT_VOL, TBM_SETPOS, TRUE, pos);
            wchar_t buf[32];
            swprintf_s(buf, L"%.1f dB", db);
            SetDlgItemTextW(hDlg, IDC_OUTPUT_VOL_VAL, buf);
        }

        // Soft limiter checkbox
        if (pThis->m_settings)
            CheckDlgButton(hDlg, IDC_SOFT_LIMITER,
                           pThis->m_settings->softLimiterEnabled ? BST_CHECKED : BST_UNCHECKED);
        if (pThis->m_gain)
            pThis->m_gain->setSoftLimiterEnabled(
                pThis->m_settings ? pThis->m_settings->softLimiterEnabled : true);

        applyDarkTheme(hDlg);
        return TRUE;
    }

    pThis = reinterpret_cast<ControlPanel*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (!pThis) return FALSE;

    switch (msg)
    {
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_SOFT_LIMITER)
        {
            bool on = IsDlgButtonChecked(hDlg, IDC_SOFT_LIMITER) == BST_CHECKED;
            if (pThis->m_gain)
                pThis->m_gain->setSoftLimiterEnabled(on);
            if (pThis->m_settings)
            {
                pThis->m_settings->softLimiterEnabled = on;
                if (pThis->m_registry) pThis->m_registry->save(*pThis->m_settings);
            }
        }
        return TRUE;
    }

    case WM_HSCROLL:
    {
        HWND hSlider = reinterpret_cast<HWND>(lParam);
        int  pos = static_cast<int>(SendMessageW(hSlider, TBM_GETPOS, 0, 0));
        float db = -60.0f + pos * 0.1f;
        if (pThis->m_gain)
        {
            pThis->m_gain->setOutputVolumeDB(db);
            if (pThis->m_settings) pThis->m_settings->outputVolumeDB = db;
            if (pThis->m_registry && pThis->m_settings)
                pThis->m_registry->save(*pThis->m_settings);
        }
        wchar_t buf[32];
        swprintf_s(buf, L"%.1f dB", db);
        SetDlgItemTextW(hDlg, IDC_OUTPUT_VOL_VAL, buf);
        return TRUE;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* pDIS = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (pDIS->CtlID == IDC_OUTPUT_METER_L || pDIS->CtlID == IDC_OUTPUT_METER_R)
        {
            float level = (pDIS->CtlID == IDC_OUTPUT_METER_L)
                          ? pThis->m_outputPeakHold_L
                          : pThis->m_outputPeakHold_R;
            float db    = (level > 1e-6f) ? 20.0f * std::log10(level) : -120.0f;
            HBRUSH hbBg = CreateSolidBrush(RGB(20, 20, 20));
            FillRect(pDIS->hDC, &pDIS->rcItem, hbBg);
            DeleteObject(hbBg);
            const int width  = pDIS->rcItem.right - pDIS->rcItem.left;
            const int filled = static_cast<int>(std::clamp(
                (db + 60.0f) / 66.0f * width, 0.0f, (float)width));
            if (filled > 0)
            {
                COLORREF col = (db > -3.0f) ? COL_RED : (db > -12.0f) ? COL_YELLOW : COL_GREEN;
                RECT fill = { pDIS->rcItem.left, pDIS->rcItem.top,
                              pDIS->rcItem.left + filled, pDIS->rcItem.bottom };
                HBRUSH hb = CreateSolidBrush(col);
                FillRect(pDIS->hDC, &fill, hb);
                DeleteObject(hb);
            }
        }
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wParam), COL_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wParam), COL_BG);
        return reinterpret_cast<INT_PTR>(g_hBrBg);
    }
    return FALSE;
}

// ============================================================================
// Advanced tab proc
// ============================================================================

INT_PTR CALLBACK ControlPanel::TabAdvancedProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ControlPanel* pThis = nullptr;
    if (msg == WM_INITDIALOG)
    {
        pThis = reinterpret_cast<ControlPanel*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pThis));

        // Buffer size dropdown — 64 is the driver minimum (matches
        // getBufferSize's advertised range; smaller values are rejected).
        static const wchar_t* bufSizes[] = {
            L"64 samples", L"128 samples", L"256 samples",
            L"512 samples", L"1024 samples", L"2048 samples"
        };
        static const int bufVals[] = { 64, 128, 256, 512, 1024, 2048 };
        for (auto* s : bufSizes)
            SendDlgItemMessageW(hDlg, IDC_ADV_BUFSIZE, CB_ADDSTRING, 0, (LPARAM)s);

        uint32_t curBuf = pThis->m_settings ? pThis->m_settings->bufferSize : 128;
        int selBuf = 1; // default 128
        for (int i = 0; i < 6; ++i) if (bufVals[i] == (int)curBuf) { selBuf = i; break; }
        SendDlgItemMessageW(hDlg, IDC_ADV_BUFSIZE, CB_SETCURSEL, selBuf, 0);

        // Sample rate dropdown
        SendDlgItemMessageW(hDlg, IDC_ADV_SAMPLERATE, CB_ADDSTRING, 0, (LPARAM)L"44100 Hz");
        SendDlgItemMessageW(hDlg, IDC_ADV_SAMPLERATE, CB_ADDSTRING, 0, (LPARAM)L"48000 Hz");
        uint32_t curSR = pThis->m_settings ? pThis->m_settings->sampleRate : 44100;
        SendDlgItemMessageW(hDlg, IDC_ADV_SAMPLERATE, CB_SETCURSEL, curSR == 48000 ? 1 : 0, 0);

        // Target dB dropdown (–6 to –18 in 0.5 steps)
        float curTarget = pThis->m_settings ? pThis->m_settings->targetPeakDBFS : -12.0f;
        int selTarget = 0, idx = 0;
        for (float db = -6.0f; db >= -24.0f; db -= 0.5f, ++idx)
        {
            wchar_t buf[16];
            swprintf_s(buf, L"%.1f dBFS", db);
            SendDlgItemMessageW(hDlg, IDC_ADV_TARGET_DB, CB_ADDSTRING, 0, (LPARAM)buf);
            if (std::abs(db - curTarget) < 0.1f) selTarget = idx;
        }
        SendDlgItemMessageW(hDlg, IDC_ADV_TARGET_DB, CB_SETCURSEL, selTarget, 0);

        // Calibration duration dropdown (3–15 sec)
        float curDur = pThis->m_settings ? pThis->m_settings->calibrationDuration : 8.0f;
        int selDur = 5;
        for (int d = 3; d <= 15; ++d)
        {
            wchar_t buf[16];
            swprintf_s(buf, L"%d sec", d);
            SendDlgItemMessageW(hDlg, IDC_ADV_CALIB_DUR, CB_ADDSTRING, 0, (LPARAM)buf);
            if (d == (int)curDur) selDur = d - 3;
        }
        SendDlgItemMessageW(hDlg, IDC_ADV_CALIB_DUR, CB_SETCURSEL, selDur, 0);

        applyDarkTheme(hDlg);
        return TRUE;
    }

    pThis = reinterpret_cast<ControlPanel*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    if (!pThis) return FALSE;

    switch (msg)
    {
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDC_ADV_RESET && HIWORD(wParam) == BN_CLICKED)
        {
            if (pThis->m_gain)
            {
                pThis->m_gain->setInputGainDB(0.0f);
                pThis->m_gain->setOutputVolumeDB(0.0f);
            }
            if (pThis->m_settings)
            {
                *pThis->m_settings = DriverSettings{};
                if (pThis->m_registry) pThis->m_registry->save(*pThis->m_settings);
            }
            MessageBoxW(hDlg, L"All settings reset to defaults.", L"OMEC Teleport ASIO", MB_OK | MB_ICONINFORMATION);
        }
        else if ((id == IDC_ADV_BUFSIZE || id == IDC_ADV_SAMPLERATE ||
                  id == IDC_ADV_TARGET_DB || id == IDC_ADV_CALIB_DUR)
                 && HIWORD(wParam) == CBN_SELCHANGE)
        {
            // Persist changes
            if (pThis->m_settings)
            {
                static const int bufVals[] = { 64, 128, 256, 512, 1024, 2048 };
                int selBuf = (int)SendDlgItemMessageW(hDlg, IDC_ADV_BUFSIZE, CB_GETCURSEL, 0, 0);
                if (selBuf >= 0 && selBuf < 6) pThis->m_settings->bufferSize = bufVals[selBuf];

                int selSR = (int)SendDlgItemMessageW(hDlg, IDC_ADV_SAMPLERATE, CB_GETCURSEL, 0, 0);
                pThis->m_settings->sampleRate = (selSR == 1) ? 48000 : 44100;

                int selTgt = (int)SendDlgItemMessageW(hDlg, IDC_ADV_TARGET_DB, CB_GETCURSEL, 0, 0);
                pThis->m_settings->targetPeakDBFS = -6.0f - selTgt * 0.5f;

                int selDur = (int)SendDlgItemMessageW(hDlg, IDC_ADV_CALIB_DUR, CB_GETCURSEL, 0, 0);
                pThis->m_settings->calibrationDuration = 3.0f + selDur;

                if (pThis->m_registry) pThis->m_registry->save(*pThis->m_settings);

                // Update latency display
                wchar_t buf[64];
                double sr = pThis->m_settings->sampleRate;
                uint32_t bs = pThis->m_settings->bufferSize;
                swprintf_s(buf, L"~%.1f ms one-way (buffer only)",
                           bs * 1000.0 / sr);
                SetDlgItemTextW(hDlg, IDC_ADV_LATENCY_DISP, buf);
            }
        }
        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam), COL_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wParam), COL_BG);
        return reinterpret_cast<INT_PTR>(g_hBrBg);
    }
    return FALSE;
}

// ============================================================================
// Status tab proc (mostly static — refreshed by timer)
// ============================================================================

INT_PTR CALLBACK ControlPanel::TabStatusProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INITDIALOG)
    {
        ControlPanel* pThis = reinterpret_cast<ControlPanel*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(pThis));

        // Set version text from the single OMEC_VERSION_TAG define
        SetDlgItemTextW(hDlg, IDC_STATUS_VERSION,
                         L"Orange OMEC Teleport ASIO Driver v" OMEC_VERSION_TAG_W);

        // Belt and braces: the dialog template also marks this control
        // NOT WS_VISIBLE; refreshStatus() shows it when an update is found.
        ShowWindow(GetDlgItem(hDlg, IDC_STATUS_UPDATE_BTN), SW_HIDE);

        // Load and display the ASIO Compatible logo
        HBITMAP hLogo = LoadBitmapW(s_hInstance, MAKEINTRESOURCEW(IDB_ASIO_LOGO));
        if (hLogo)
            SendDlgItemMessageW(hDlg, IDC_STATUS_ICON, STM_SETIMAGE,
                                IMAGE_BITMAP, reinterpret_cast<LPARAM>(hLogo));

        applyDarkTheme(hDlg);
        return TRUE;
    }

    switch (msg)
    {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STATUS_UPDATE_BTN && HIWORD(wParam) == BN_CLICKED)
        {
            // url is only ever a validated https://github.com/.../releases/ link
            if (g_update.available.load(std::memory_order_acquire) && g_update.url[0])
                ShellExecuteW(nullptr, L"open", g_update.url,
                              nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        return FALSE;   // don't claim notifications we don't handle

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        // Highlight the update notification text in blue
        if (GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == IDC_STATUS_UPDATE)
            SetTextColor(hdc, COL_HIGHLIGHT);
        else
            SetTextColor(hdc, COL_TEXT);
        SetBkColor(hdc, COL_BG);
        return reinterpret_cast<INT_PTR>(g_hBrBg);
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wParam), COL_TEXT);
        SetBkColor(reinterpret_cast<HDC>(wParam), COL_BG);
        return reinterpret_cast<INT_PTR>(g_hBrBg);
    }
    return FALSE;
}

// ============================================================================
// Calibration dialog proc (unused directly — calibration runs in main tab)
// ============================================================================
INT_PTR CALLBACK ControlPanel::CalibrationProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hDlg; (void)msg; (void)wParam; (void)lParam;
    return FALSE;
}
