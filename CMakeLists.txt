cmake_minimum_required(VERSION 3.20)
project(openxr_eye_capture VERSION 0.1.0 LANGUAGES C CXX)

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

# --- core: everything the layer and the encoder self-test share (recorder, audio, copy, st3d, log, config) ---
add_library(eye_capture_core STATIC
    src/log.cpp
    src/config.cpp
    src/frame_source.cpp
    src/source_d3d11.cpp
    src/source_d3d12.cpp
    src/source_vulkan.cpp
    src/recorder.cpp
    src/head_smoothing.cpp
    src/audio_capture.cpp
    src/st3d.cpp)
target_include_directories(eye_capture_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(eye_capture_core PUBLIC
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    EYE_CAPTURE_VERSION="${PROJECT_VERSION}")
target_compile_options(eye_capture_core PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(eye_capture_core PUBLIC
    d3d11 d3d12 dxgi d3dcompiler mfplat mfreadwrite mfuuid ole32 strmiids shell32 winmm)

# --- the API layer DLL ---
add_library(eye_capture SHARED
    src/layer.cpp
    src/control.cpp)
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

# --- tray control app (optional; the layer works without it) ---
add_executable(EyeCaptureTray WIN32 tray/tray.cpp tray/postprocess.cpp)
target_compile_options(EyeCaptureTray PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(EyeCaptureTray PRIVATE eye_capture_core comctl32)

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

    # Head-smoothing filter unit test (CPU only).
    add_executable(head_smoothing_test tests/head_smoothing_test.cpp)
    target_compile_options(head_smoothing_test PRIVATE ${EYE_WARN_FLAGS})
    target_link_libraries(head_smoothing_test PRIVATE eye_capture_core)
    target_include_directories(head_smoothing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

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
