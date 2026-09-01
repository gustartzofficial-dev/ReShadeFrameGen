cmake_minimum_required(VERSION 3.20)

project(ReShadeDX11FrameGenAddon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ReShade is used as an SDK/header dependency here.
# Keep the project's existing SDK pin so this update changes the frame-generation code only.
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

# Hybrid wrapper includes the existing src/reshade_dx11_framegen.cpp directly. Compile ONLY the
# wrapper here; compiling both files separately would duplicate the original add-on symbols.
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
)

set_target_properties(ReShadeDX11FrameGen PROPERTIES
    OUTPUT_NAME "ReShadeDX11FrameGen"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET ReShadeDX11FrameGen POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/addon"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ReShadeDX11FrameGen>"
        "${CMAKE_BINARY_DIR}/addon/ReShadeDX11FrameGen.addon"
    COMMENT "Creating .addon copy"
)
