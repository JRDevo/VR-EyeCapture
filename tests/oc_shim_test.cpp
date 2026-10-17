// oc_shim_test -- a DLL whose exports are only forwarders to openvr_api_oc.dll, like the OpenComposite switcher's
// openvr_api.dll shim. Never loaded: tests/steamvr_install_test reads its export table.
#include <windows.h>
#pragma comment(linker, "/export:VR_InitInternal2=openvr_api_oc.VR_InitInternal2")
#pragma comment(linker, "/export:VR_GetGenericInterface=openvr_api_oc.VR_GetGenericInterface")
#pragma comment(linker, "/export:VR_ShutdownInternal=openvr_api_oc.VR_ShutdownInternal")
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
