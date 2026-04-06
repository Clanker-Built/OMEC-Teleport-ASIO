#pragma once
//-----------------------------------------------------------------------------
// Guids.h — GUIDs for OMEC Teleport ASIO Driver
//-----------------------------------------------------------------------------

// ASIO Driver CLSID — registered under HKLM\SOFTWARE\ASIO and HKCR\CLSID
// {4DD0AC43-EB4E-4358-B0B1-1DBC797E0498}
DEFINE_GUID(CLSID_OmecTeleportASIO,
    0x4dd0ac43, 0xeb4e, 0x4358, 0xb0, 0xb1, 0x1d, 0xbc, 0x79, 0x7e, 0x04, 0x98);

// Device Interface GUID — written to INF [Dev_AddReg], used by DeviceEnumerator
// {5432A101-0630-4BBE-BD22-5649D8D1008C}
DEFINE_GUID(GUID_OmecTeleportDeviceInterface,
    0x5432a101, 0x0630, 0x4bbe, 0xbd, 0x22, 0x56, 0x49, 0xd8, 0xd1, 0x00, 0x8c);
