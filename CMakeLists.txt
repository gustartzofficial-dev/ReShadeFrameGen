cmake_minimum_required(VERSION 3.24)

project(ReShadeFrameGen LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ReShadeFrameGen DLSS-G Host is Windows-only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This first DLSS-G host build is 64-bit only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# DLSS5-Feeder and the current RenoDX DLSS work target ReShade 6.8+.
FetchContent_Declare(
    reshade_sdk
    GIT_REPOSITORY https://github.com/crosire/reshade.git
    GIT_TAG v6.8.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(reshade_sdk)
if(NOT reshade_sdk_POPULATED)
    FetchContent_Populate(reshade_sdk)
endif()

# Headers only. No NVIDIA binaries are redistributed by this repository.
FetchContent_Declare(
    streamline_sdk
    GIT_REPOSITORY https://github.com/NVIDIA-RTX/Streamline.git
    GIT_TAG v2.12.0
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(streamline_sdk)
if(NOT streamline_sdk_POPULATED)
    FetchContent_Populate(streamline_sdk)
endif()

add_library(ReShadeFrameGen MODULE
    src/addon.cpp
    src/dependency_probe.cpp
    src/feeder_probe.cpp
    src/streamline_host.cpp
)

target_include_directories(ReShadeFrameGen PRIVATE
    ${reshade_sdk_SOURCE_DIR}/include
    ${reshade_sdk_SOURCE_DIR}/deps/imgui
    ${streamline_sdk_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(ReShadeFrameGen PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(ReShadeFrameGen PRIVATE /W4 /permissive- /EHsc)
endif()

target_link_libraries(ReShadeFrameGen PRIVATE user32)

# Emit the actual ReShade loader-facing file directly. No intermediate release DLL is packaged.
set_target_properties(ReShadeFrameGen PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ReShadeFrameGen"
    SUFFIX ".addon64"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
)

foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set_target_properties(ReShadeFrameGen PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/addon/${cfg}"
        LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${CMAKE_BINARY_DIR}/addon/${cfg}"
    )
endforeach()
