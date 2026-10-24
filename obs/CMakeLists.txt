# obs-eyecapture.dll -- the VR EyeCapture OBS Studio source plugin, built on its own from this folder (GPL-2.0-or-later).
# Only the libobs HEADERS of OBS 31.0.0 are fetched (shallow, no submodules, OBS itself is not configured or built); the
# import library for obs.dll is generated from obs.def (only the exports the plugin uses), so no OBS binaries are needed.
cmake_minimum_required(VERSION 3.20)
project(obs_eyecapture LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(EYE_WARN_FLAGS /W3 /permissive- /Zc:__cplusplus /utf-8)

include(FetchContent)
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
    COMMAND ${CMAKE_AR} /nologo /def:${CMAKE_CURRENT_SOURCE_DIR}/obs.def /machine:x64 /out:${EYE_OBS_IMPLIB}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/obs.def
    COMMENT "obs.lib import library from obs.def")
add_custom_target(eye_obs_implib DEPENDS ${EYE_OBS_IMPLIB})

add_library(obs-eyecapture MODULE plugin.cpp live_consumer.cpp)
add_dependencies(obs-eyecapture eye_obs_implib)
# the public libobs headers need none of the files OBS generates at configure time (obsconfig.h is internal only)
target_include_directories(obs-eyecapture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${obs_studio_SOURCE_DIR}/libobs)
target_compile_definitions(obs-eyecapture PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(obs-eyecapture PRIVATE ${EYE_WARN_FLAGS})
target_link_libraries(obs-eyecapture PRIVATE d3d11 dxgi ${EYE_OBS_IMPLIB})
