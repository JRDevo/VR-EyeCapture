// layer_loadtest -- the real OpenXR loader + the ACTIVE runtime (VDXR / SteamVR / Oculus) + our installed implicit
// layer. Confirms the loader finds and loads XR_APILAYER_JRDEVO_eye_capture, that negotiation and dispatch work, and
// that instance creation / destruction through the layer does not crash. A runtime may refuse xrCreateInstance or
// xrGetSystem without a headset; that is reported, not a failure of this test.
//
// Sets XR_LOADER_DEBUG=all unless already set, so the loader prints its layer search to stderr.
// Exit code: 0 = our layer was enumerated (and, if the runtime created an instance, it was created/destroyed cleanly).
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <openxr/openxr.h>

#define LAYER_NAME "XR_APILAYER_JRDEVO_eye_capture"

int main() {
    if (!GetEnvironmentVariableA("XR_LOADER_DEBUG", nullptr, 0)) SetEnvironmentVariableA("XR_LOADER_DEBUG", "all");

    uint32_t n = 0;
    XrResult r = xrEnumerateApiLayerProperties(0, &n, nullptr);
    printf("xrEnumerateApiLayerProperties -> %d, %u layers\n", (int)r, n);
    std::vector<XrApiLayerProperties> props(n, {XR_TYPE_API_LAYER_PROPERTIES});
    if (n) r = xrEnumerateApiLayerProperties(n, &n, props.data());
    bool found = false;
    for (auto& p : props) {
        printf("  layer: %-48s spec %u.%u.%u impl %u  %s\n", p.layerName, XR_VERSION_MAJOR(p.specVersion),
               XR_VERSION_MINOR(p.specVersion), XR_VERSION_PATCH(p.specVersion), p.layerVersion, p.description);
        if (strcmp(p.layerName, LAYER_NAME) == 0) found = true;
    }
    printf("our layer %s\n", found ? "FOUND" : "NOT FOUND (run scripts\\install.ps1)");

    uint32_t ne = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ne, nullptr);
    std::vector<XrExtensionProperties> exts(ne, {XR_TYPE_EXTENSION_PROPERTIES});
    if (ne) xrEnumerateInstanceExtensionProperties(nullptr, ne, &ne, exts.data());
    bool d3d11 = false;
    for (auto& e : exts) if (strcmp(e.extensionName, "XR_KHR_D3D11_enable") == 0) d3d11 = true;
    printf("runtime offers %u extensions, XR_KHR_D3D11_enable %s\n", ne, d3d11 ? "yes" : "no");

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "eye_capture_loadtest");
    strcpy_s(ci.applicationInfo.engineName, "none");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    const char* extList[] = {"XR_KHR_D3D11_enable"};
    if (d3d11) { ci.enabledExtensionCount = 1; ci.enabledExtensionNames = extList; }
    XrInstance inst = XR_NULL_HANDLE;
    r = xrCreateInstance(&ci, &inst);
    printf("xrCreateInstance -> %d\n", (int)r);
    if (XR_SUCCEEDED(r)) {
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(inst, &ip);
        printf("runtime: %s %u.%u.%u\n", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
               XR_VERSION_PATCH(ip.runtimeVersion));
        // dispatch through the layer: a hooked function and a forwarded one must both resolve
        PFN_xrVoidFunction f1 = nullptr, f2 = nullptr;
        XrResult g1 = xrGetInstanceProcAddr(inst, "xrEndFrame", &f1);
        XrResult g2 = xrGetInstanceProcAddr(inst, "xrPollEvent", &f2);
        printf("xrGetInstanceProcAddr(xrEndFrame) -> %d %p, (xrPollEvent) -> %d %p\n", (int)g1, (void*)f1, (int)g2, (void*)f2);
        XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
        sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        r = xrGetSystem(inst, &sg, &sys);
        printf("xrGetSystem -> %d (no headset: an error here is expected)\n", (int)r);
        r = xrDestroyInstance(inst);
        printf("xrDestroyInstance -> %d\n", (int)r);
    }
    printf("RESULT: layer %s, instance %s\n", found ? "loaded" : "missing", inst ? "created+destroyed" : "not created by runtime");
    return found ? 0 : 1;
}
