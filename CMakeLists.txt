cmake_minimum_required(VERSION 3.20)
project(openxr_eye_capture VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# /EHa: a hardware fault inside the capture path unwinds C++ objects (locks) and is caught in xrEndFrame, which then
# disables capture for the session instead of taking the game down.
string(REPLACE "/EHsc" "/EHa" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# --- OpenXR SDK --- headers for the layer, plus the loader (openxr_loader.dll) for the tests only.
# The layer itself never links the loader: the loader loads the layer. Fetched at configure time
# (needs git + internet on the first configure).
include(FetchContent)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
FetchContent_Declare(openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.62   # 57af7fc61f9f2d492580cb28aab6d0ea59d8d417
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(openxr_sdk)

set(EYE_WARN_FLAGS /W3 /permissive- /Zc:__cplusplus /utf-8)
# CI sets EYE_WERROR=ON: any compiler warning in OUR sources fails the build (the OpenXR SDK targets are not affected).
option(EYE_WERROR "Treat compiler warnings in our sources as errors (/WX)" OFF)
if (EYE_WERROR)
    list(APPEND EYE_WARN_FLAGS /WX)
endif()

# --- core: everything the layer and the encoder self-test share (recorder, audio, copy, st3d, log, config) ---
add_library(eye_capture_core STATIC
    src/log.cpp
    src/config.cpp
    src/hotkey.cpp
    src/frame_source.cpp
    src/source_d3d11.cpp
    src/source_d3d12.cpp
    src/source_vulkan.cpp
    src/recorder.cpp
    src/live_share.cpp
    src/reproject.cpp
    src/head_smoothing.cpp
    src/dome.cpp
    src/indicator.cpp
    src/overlay.cpp
    src/audio_capture.cpp
    src/st3d.cpp
    src/control.cpp
    src/capture_common.cpp)
target_include_directories(eye_capture_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(eye_capture_core PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    EYE_CAPTURE_VERSION="${PROJECT_VERSION}")
target_compile_options(eye_capture_core PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(eye_capture_core PUBLIC
    d3d11 d3d12 dxgi d3dcompiler mfplat mfreadwrite mfuuid ole32 strmiids shell32 winmm OpenXR::headers)

# --- the API layer DLL ---
add_library(eye_capture SHARED
    src/layer.cpp)
target_compile_options(eye_capture PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(eye_capture PRIVATE eye_capture_core OpenXR::headers)
target_compile_definitions(eye_capture PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12)

# Manifest next to the DLL (library_path is relative to the manifest).
set(EYE_LAYER_NAME "XR_APILAYER_JRDEVO_eye_capture")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/${EYE_LAYER_NAME}.json.in
               ${CMAKE_CURRENT_BINARY_DIR}/${EYE_LAYER_NAME}.json @ONLY)
add_custom_command(TARGET eye_capture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/${EYE_LAYER_NAME}.json
            $<TARGET_FILE_DIR:eye_capture>/${EYE_LAYER_NAME}.json)

# --- SteamVR (OpenVR) proxy: openvr_api.dll (src/openvr/, docs/STEAMVR_DESIGN.md) ---
# Placed by the tray's "SteamVR games" Enable where a game's Valve openvr_api.dll was (that one is renamed
# openvr_api_real.dll); never linked against Valve's import library (third_party/openvr/openvr.h: types only).
# accessor_stubs.asm: register-preserving jump stubs for the exports of unknown signature (VRHeadsetView, ...).
enable_language(ASM_MASM)
add_library(openvr_proxy SHARED
    src/openvr/proxy_exports.cpp
    src/openvr/openvr_capture.cpp
    src/openvr/openvr_hooks.cpp
    src/openvr/accessor_stubs.asm
    src/openvr/openvr_api.def)
set_target_properties(openvr_proxy PROPERTIES OUTPUT_NAME openvr_api)
target_compile_options(openvr_proxy PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${EYE_WARN_FLAGS}>)
target_link_libraries(openvr_proxy PRIVATE eye_capture_core advapi32)

# --- SteamVR quick mode: EyeCaptureMirror.exe (src/openvr/mirror_main.cpp) records SteamVR's mirror texture through Valve's own
# openvr_api.dll v2.15.6 (third_party/openvr/bin/win64, BSD-3), copied to <bin>/valve/ (the bin folder's openvr_api.dll is
# our proxy). The tray starts it while steamvr_mode = quick and vrserver.exe runs.
add_executable(EyeCaptureMirror WIN32 src/openvr/mirror_main.cpp src/openvr/openvr_hooks.cpp)
target_compile_options(EyeCaptureMirror PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(EyeCaptureMirror PRIVATE eye_capture_core advapi32 psapi)
add_custom_command(TARGET EyeCaptureMirror POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:EyeCaptureMirror>/valve
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr/bin/win64/openvr_api.dll
            $<TARGET_FILE_DIR:EyeCaptureMirror>/valve/openvr_api.dll)

# --- tray control app (required: the layer is dormant without it; it also registers the layer, tray/install.cpp) ---
# tray/steamvr_ui.cpp + tray/steamvr_games.cpp: the "SteamVR games" window (enable / disable the per-game proxy);
# src/openvr/openvr_hooks.cpp for the sidecar text and the home-key names.
add_executable(EyeCaptureTray WIN32 tray/tray.cpp tray/postprocess.cpp tray/settings.cpp tray/install.cpp tray/input.cpp
    tray/steamvr_ui.cpp tray/steamvr_games.cpp src/openvr/openvr_hooks.cpp tray/tray.rc)
target_compile_options(EyeCaptureTray PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(EyeCaptureTray PRIVATE eye_capture_core comctl32 comdlg32 dinput8 dxguid advapi32 version)
target_include_directories(EyeCaptureTray PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tray)

# --- live share consumer (obs/live_consumer.cpp): no libobs; used by the OBS plugin and tests/live_share_test ---
add_library(eye_live_consumer STATIC obs/live_consumer.cpp)
target_include_directories(eye_live_consumer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/obs ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(eye_live_consumer PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(eye_live_consumer PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(eye_live_consumer PUBLIC d3d11 dxgi)

# --- OBS Studio source plugin obs-eyecapture.dll (optional; obs/README.md) ---
# Only the libobs HEADERS of OBS 31.0.0 are fetched (shallow, no submodules, OBS itself is not configured or built); the
# import library for obs.dll is generated from obs/obs.def (only the exports the plugin uses), so no OBS binaries are
# needed to build.
option(EYE_BUILD_OBS_PLUGIN "Build the OBS Studio source plugin obs-eyecapture.dll (fetches the libobs headers)" OFF)
if (EYE_BUILD_OBS_PLUGIN)
    FetchContent_Declare(obs_studio
        GIT_REPOSITORY https://github.com/obsproject/obs-studio.git
        GIT_TAG        31.0.0
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES ""
        SOURCE_SUBDIR  headers-only-not-built)   # no CMakeLists.txt there: populated, never added to the build
    FetchContent_MakeAvailable(obs_studio)
    set(EYE_OBS_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/obs_implib/obs.lib)
    add_custom_command(OUTPUT ${EYE_OBS_IMPLIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/obs_implib
        COMMAND ${CMAKE_AR} /nologo /def:${CMAKE_CURRENT_SOURCE_DIR}/obs/obs.def /machine:x64 /out:${EYE_OBS_IMPLIB}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/obs/obs.def
        COMMENT "obs.lib import library from obs/obs.def")
    add_custom_target(eye_obs_implib DEPENDS ${EYE_OBS_IMPLIB})
    add_library(obs-eyecapture MODULE obs/plugin.cpp)
    add_dependencies(obs-eyecapture eye_obs_implib)
    # the public libobs headers need none of the files OBS generates at configure time (obsconfig.h is internal only)
    target_include_directories(obs-eyecapture PRIVATE ${obs_studio_SOURCE_DIR}/libobs)
    target_compile_options(obs-eyecapture PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(obs-eyecapture PRIVATE eye_live_consumer ${EYE_OBS_IMPLIB})
endif()

# --- tests ---
option(EYE_BUILD_TESTS "Build the self-tests (encoder self-test, loader test, mock runtime end-to-end)" ON)
if (EYE_BUILD_TESTS)
    # Encoder self-test: drives the same Recorder + eye copy the layer uses, with its own D3D11 device.
    add_executable(encoder_selftest tests/encoder_selftest.cpp)
    target_compile_options(encoder_selftest PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(encoder_selftest PRIVATE eye_capture_core)
    target_include_directories(encoder_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # Plays the layer's side of the tray IPC (ready/recording/stopping/saved) on a private block, for testing the tray's post-processing.
    add_executable(status_sim tests/status_sim.cpp)
    target_compile_options(status_sim PRIVATE ${EYE_WARN_FLAGS})
    target_compile_definitions(status_sim PRIVATE UNICODE _UNICODE NOMINMAX)
    target_include_directories(status_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    # Stand-in for ffmpeg.exe (sleeps in ticks, copies -i to the output): the tray's post_when hold test, no encoder needed.
    add_executable(fake_ffmpeg tests/fake_ffmpeg.cpp)
    target_compile_options(fake_ffmpeg PRIVATE ${EYE_WARN_FLAGS})
    target_compile_definitions(fake_ffmpeg PRIVATE UNICODE _UNICODE NOMINMAX)

    # Control thread + ControlPublishFinal on a private status block (no GPU / game): the "saved <file>" shutdown race.
    add_executable(control_final_test tests/control_final_test.cpp)
    target_compile_options(control_final_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(control_final_test PRIVATE eye_capture_core)

    # Settings window: config.ini writer (in-place edit, validation, hotkey names) + driver for the real dialog (no GPU).
    add_executable(settings_test tests/settings_test.cpp tray/settings.cpp tray/install.cpp tray/input.cpp)
    target_compile_options(settings_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(settings_test PRIVATE eye_capture_core comctl32 advapi32 dinput8 dxguid)
    target_include_directories(settings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tray)

    # Layer registration (tray/install.cpp) on test keys under HKCU (no admin, never the real OpenXR keys).
    add_executable(install_test tests/install_test.cpp tray/install.cpp)
    target_compile_options(install_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(install_test PRIVATE eye_capture_core advapi32)
    target_include_directories(install_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tray)

    # SteamVR games (tray/steamvr_games.cpp): scan / PE checks / enable / disable / status on a fake game tree (CPU only).
    add_executable(steamvr_install_test tests/steamvr_install_test.cpp tray/steamvr_games.cpp src/openvr/openvr_hooks.cpp)
    target_compile_options(steamvr_install_test PRIVATE ${EYE_WARN_FLAGS})
    target_compile_definitions(steamvr_install_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        EYE_CAPTURE_VERSION="${PROJECT_VERSION}" EYE_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        EYE_VALVE_DLL="${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr/bin/win64/openvr_api.dll")
    target_include_directories(steamvr_install_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tray ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(steamvr_install_test PRIVATE advapi32 version)
    add_dependencies(steamvr_install_test openvr_proxy mock_openvr oc_shim_test)
    # A DLL with nothing but forwarders to openvr_api_oc.dll (like OpenComposite's switcher shim), for the detection test.
    add_library(oc_shim_test SHARED tests/oc_shim_test.cpp)

    # SteamVR proxy helpers (src/openvr/openvr_hooks.cpp): versions, slot table, vtable patch, FnTable copy, matrices,
    # fov, bounds, colour space, home discovery (CPU only).
    add_executable(openvr_hook_test tests/openvr_hook_test.cpp src/openvr/openvr_hooks.cpp)
    target_compile_options(openvr_hook_test PRIVATE ${EYE_WARN_FLAGS})
    target_compile_definitions(openvr_hook_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(openvr_hook_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(openvr_hook_test PRIVATE advapi32)

    # Mock Valve openvr_api.dll (installed as openvr_api_real.dll next to the proxy by tests/run_steamvr.ps1) + a fake
    # OpenVR game that drives the proxy (D3D11, every texture layout, C++ and FnTable interfaces).
    add_library(mock_openvr SHARED tests/mock_openvr.cpp)
    target_compile_options(mock_openvr PRIVATE ${EYE_WARN_FLAGS})
    target_compile_definitions(mock_openvr PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(mock_openvr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(mock_openvr PRIVATE d3d11)
    add_executable(openvr_e2e tests/openvr_e2e.cpp src/head_smoothing.cpp)   # ComputeSymmetricFov: the expected size
    target_compile_options(openvr_e2e PRIVATE ${EYE_WARN_FLAGS})
    target_compile_definitions(openvr_e2e PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(openvr_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(openvr_e2e PRIVATE d3d11 dxgi winmm)
    add_dependencies(openvr_e2e openvr_proxy mock_openvr)

    # Head-smoothing filter unit test (CPU only).
    add_executable(head_smoothing_test tests/head_smoothing_test.cpp)
    target_compile_options(head_smoothing_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(head_smoothing_test PRIVATE eye_capture_core)
    target_include_directories(head_smoothing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # In-headset indicator logic unit test (CPU only).
    add_executable(indicator_test tests/indicator_test.cpp)
    target_compile_options(indicator_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(indicator_test PRIVATE eye_capture_core)

    # record_overlays geometry unit test (CPU only): projection, winding / culling side, pixel box, format families.
    add_executable(overlay_test tests/overlay_test.cpp)
    target_compile_options(overlay_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(overlay_test PRIVATE eye_capture_core)

    # Loader test: the real OpenXR loader + whatever runtime is active (VDXR here) + our installed layer.
    add_executable(layer_loadtest tests/layer_loadtest.cpp)
    target_compile_options(layer_loadtest PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(layer_loadtest PRIVATE OpenXR::openxr_loader OpenXR::headers)
    target_compile_definitions(layer_loadtest PRIVATE UNICODE _UNICODE NOMINMAX)

    # Mock OpenXR runtime (D3D11 swapchains, no headset) + an app that drives the full frame loop
    # through the loader and our layer, for every swapchain layout the layer must handle.
    add_library(mock_runtime SHARED tests/mock_runtime.cpp)
    target_compile_options(mock_runtime PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(mock_runtime PRIVATE OpenXR::headers d3d11 d3d12 dxgi)
    target_include_directories(mock_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(mock_runtime PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 NOMINMAX)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/tests/mock_runtime.json.in
                   ${CMAKE_CURRENT_BINARY_DIR}/mock_runtime.json COPYONLY)

    # Live share consumer test (obs/live_consumer.cpp, the plugin's code) against layer_e2e + the layer: tests/run_live.ps1.
    add_executable(live_share_test tests/live_share_test.cpp src/head_smoothing.cpp)   # ComputeSymmetricFov: the expected size
    target_compile_options(live_share_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(live_share_test PRIVATE eye_live_consumer)
    target_include_directories(live_share_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    add_executable(layer_e2e tests/layer_e2e.cpp)
    target_compile_options(layer_e2e PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(layer_e2e PRIVATE OpenXR::openxr_loader OpenXR::headers d3d11 d3d12 dxgi d3dcompiler)
    target_include_directories(layer_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(layer_e2e PRIVATE XR_USE_PLATFORM_WIN32 XR_USE_GRAPHICS_API_D3D11 XR_USE_GRAPHICS_API_D3D12 UNICODE _UNICODE NOMINMAX)
    add_dependencies(layer_e2e mock_runtime eye_capture)

    foreach (t layer_loadtest layer_e2e)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:OpenXR::openxr_loader> $<TARGET_FILE_DIR:${t}>)
    endforeach()
    add_custom_command(TARGET mock_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/mock_runtime.json $<TARGET_FILE_DIR:mock_runtime>/mock_runtime.json)
endif()
