#pragma once
//-----------------------------------------------------------------------------
// resource.h — Dialog and control IDs for OMEC Teleport ASIO Control Panel
//-----------------------------------------------------------------------------

// Dialogs
#define IDD_MAIN_DIALOG       101
#define IDD_TAB_STATUS        102
#define IDD_TAB_INPUT         103
#define IDD_TAB_OUTPUT        104
#define IDD_TAB_ADVANCED      105
#define IDD_CALIBRATION       106

// Tab control
#define IDC_TAB               201

// --- Device Status tab ---
#define IDC_STATUS_DEVICE     202
#define IDC_STATUS_SAMPLERATE 203
#define IDC_STATUS_BUFFERSIZE 204
#define IDC_STATUS_LATENCY_IN 205
#define IDC_STATUS_LATENCY_OUT 206
#define IDC_STATUS_ICON       207

// --- Input Controls tab ---
#define IDC_INPUT_METER_L     210
#define IDC_INPUT_METER_R     211
#define IDC_INPUT_GAIN_L      212
#define IDC_INPUT_GAIN_R      213
#define IDC_INPUT_GAIN_L_VAL  214
#define IDC_INPUT_GAIN_R_VAL  215
#define IDC_INPUT_LINK        216
#define IDC_AUTOSET_BTN       217
#define IDC_AUTOSET_PROGRESS  218
#define IDC_AUTOSET_STATUS    219

// --- Output Controls tab ---
#define IDC_OUTPUT_METER_L    220
#define IDC_OUTPUT_METER_R    221
#define IDC_OUTPUT_VOL        222
#define IDC_OUTPUT_VOL_VAL    223
#define IDC_DIRECT_MON        224
#define IDC_SOFT_LIMITER      225

// --- Advanced tab ---
#define IDC_ADV_BUFSIZE       230
#define IDC_ADV_SAMPLERATE    231
#define IDC_ADV_LATENCY_DISP  232
#define IDC_ADV_RESET         233
#define IDC_ADV_TARGET_DB     234
#define IDC_ADV_CALIB_DUR     235

// --- Calibration dialog ---
#define IDC_CALIB_METER       240
#define IDC_CALIB_COUNTDOWN   241
#define IDC_CALIB_RESULT      242
#define IDC_CALIB_ACCEPT      243
#define IDC_CALIB_CANCEL_BTN  244
#define IDC_CALIB_INSTRUCTION 245

// --- Update notification & version (Device Status tab) ---
#define IDC_STATUS_UPDATE     208
#define IDC_STATUS_UPDATE_BTN 209
#define IDC_STATUS_VERSION    250

// Bitmaps
#define IDB_ASIO_LOGO         280

// ==========================================================================
// VERSION — change ONLY these defines for each release.
// Everything else (VERSIONINFO, About text, update check) reads from here.
// ==========================================================================
#define OMEC_VERSION_MAJOR    1
#define OMEC_VERSION_MINOR    5
#define OMEC_VERSION_PATCH    0
#define OMEC_VERSION_TAG      "1.5"
#define OMEC_VERSION_TAG_W    L"1.5"
#define OMEC_VERSION_FILE_STR "1.5.0.0"

// Version info string table
#define IDS_DRIVER_NAME       301
#define IDS_DRIVER_VERSION    302
