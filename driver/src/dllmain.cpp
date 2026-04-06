//-----------------------------------------------------------------------------
// dllmain.cpp — COM factory and registration for OmecTeleport ASIO Driver
//
// The Steinberg SDK's dllentry.cpp provides DllEntryPoint (exported as DllMain
// via driver.def), DllGetClassObject, and DllCanUnloadNow.
// register.cpp provides RegisterAsioDriver / UnregisterAsioDriver.
// We define g_Templates[], g_cTemplates, DllRegisterServer, DllUnregisterServer,
// and g_hInst here.
//-----------------------------------------------------------------------------

// Must include COM headers before any Steinberg SDK headers
#include <windows.h>
#include <objbase.h>

// Steinberg SDK
#include "combase.h"

// Our driver
#include "OmecTeleportASIO.h"
#include "Guids.h"
#include "ControlPanel.h"

// ---- Steinberg SDK globals -------------------------------------------------

// g_hInst: declared extern in combase.h; dllentry.cpp uses its own 'hinstance'
// variable.  We define g_hInst here so the linker is satisfied.
HINSTANCE g_hInst = nullptr;

// The hinstance defined inside dllentry.cpp is used internally by the SDK.
// We expose it here so DllRegisterServer can pass the DLL filename.
extern HINSTANCE hinstance;

// ---- Factory template array (consumed by dllentry.cpp's DllGetClassObject) -

CFactoryTemplate g_Templates[] = {
    {
        L"OmecTeleport ASIO",
        &CLSID_OmecTeleportASIO,
        OmecTeleportASIO::CreateInstance,
        nullptr    // no init routine
    }
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// ---- External from register.cpp --------------------------------------------

extern LONG RegisterAsioDriver(CLSID, char*, char*, char*, char*);
extern LONG UnregisterAsioDriver(CLSID, char*, char*);

// ---- Registration ----------------------------------------------------------

HRESULT __stdcall DllRegisterServer()
{
    // Store hInstance for control panel use
    ControlPanel::s_hInstance = hinstance;
    g_hInst = hinstance;

    LONG rc = RegisterAsioDriver(
        CLSID_OmecTeleportASIO,         // CLSID
        "OmecTeleportASIO.dll",         // DLL filename (must match actual .dll name)
        "OmecTeleport ASIO",            // Registry subkey under HKLM\SOFTWARE\ASIO
        "Orange OMEC Teleport ASIO",    // Description
        "Apartment"                     // Threading model
    );

    if (rc != 0)
    {
        char msg[128];
        sprintf_s(msg, "DllRegisterServer failed (code %ld). Run as Administrator.", rc);
        MessageBoxA(nullptr, msg, "OMEC Teleport ASIO", MB_OK | MB_ICONERROR);
        return E_FAIL;
    }
    return S_OK;
}

HRESULT __stdcall DllUnregisterServer()
{
    LONG rc = UnregisterAsioDriver(
        CLSID_OmecTeleportASIO,
        "OmecTeleportASIO.dll",
        "OmecTeleport ASIO"
    );

    if (rc != 0)
    {
        char msg[128];
        sprintf_s(msg, "DllUnregisterServer failed (code %ld).", rc);
        MessageBoxA(nullptr, msg, "OMEC Teleport ASIO", MB_OK | MB_ICONWARNING);
        return E_FAIL;
    }
    return S_OK;
}

// Notified by dllentry.cpp's DllEntryPoint on DLL_PROCESS_ATTACH.
// dllentry.cpp uses its own 'hinstance' variable; we sync g_hInst here.
// The DllInitClasses mechanism calls this indirectly, but for direct attach
// notification we also use the CFactoryTemplate init routine.
// A cleaner approach: override DllEntryPoint is not practical with the SDK,
// so we simply set g_hInst in DllRegisterServer above where it's first needed.
