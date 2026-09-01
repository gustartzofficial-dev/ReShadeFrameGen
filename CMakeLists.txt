cmake_minimum_required(VERSION 3.20)

project(ReShadeDX11FrameGenAddon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ReShade SDK/header dependency. Keep the existing SDK pin for compatibility.
include(FetchContent)
FetchContent_Declare(
    reshade_sdk
    GIT_REPOSITORY https://github.com/crosire/reshade.git
    GIT_TAG v6.7.3
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(reshade_sdk)
if(NOT reshade_sdk_POPULATED)
    FetchContent_Populate(reshade_sdk)
endif()

# The hybrid/Frankenstein wrapper includes the repository's existing
# src/reshade_dx11_framegen.cpp directly. Compile ONLY this wrapper to avoid
# duplicate definitions while preserving the old implementation as fallback.
add_library(ReShadeDX11FrameGen SHARED
    src/reshade_dx11_framegen_hybrid.cpp
)

target_include_directories(ReShadeDX11FrameGen PRIVATE
    ${reshade_sdk_SOURCE_DIR}/include
    ${reshade_sdk_SOURCE_DIR}/deps/imgui
)

target_compile_definitions(ReShadeDX11FrameGen PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(ReShadeDX11FrameGen PRIVATE
    d3d11
    dxgi
    d3dcompiler
    user32
    dwmapi
)

# Emit the PE DLL directly with ReShade's add-on extension. This avoids producing
# a second runtime .dll that looks like a dependency but is actually the same binary.
set_target_properties(ReShadeDX11FrameGen PROPERTIES
    PREFIX ""
    SUFFIX ".addon"
    OUTPUT_NAME "ReShadeDX11FrameGen"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
)
