cmake_minimum_required(VERSION 3.20)
project(ReShadeDX11FrameGenAddon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  reshade
  GIT_REPOSITORY https://github.com/crosire/reshade.git
  GIT_TAG main
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(reshade)

add_library(ReShadeDX11FrameGen SHARED
  src/reshade_dx11_framegen.cpp
)

target_include_directories(ReShadeDX11FrameGen PRIVATE
  ${reshade_SOURCE_DIR}/include
  ${reshade_SOURCE_DIR}/deps/imgui
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
  COMMAND ${CMAKE_COMMAND} -E copy
          "$<TARGET_FILE:ReShadeDX11FrameGen>"
          "${CMAKE_BINARY_DIR}/bin/ReShadeDX11FrameGen.addon"
  COMMENT "Creating .addon copy"
)
