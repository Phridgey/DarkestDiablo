cmake_minimum_required(VERSION 3.29)
project(darkestdiablo_plugin LANGUAGES CXX RC)

# D2RLoader Plugin SDK (MIT license). Pinned to 0.3.0, the release that matches D2RLoader 1.3.1.
include(FetchContent)
set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(D2RLPluginSDK
  GIT_REPOSITORY https://github.com/D2RLoader/PluginSDK.git
  GIT_TAG v0.3.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(D2RLPluginSDK)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no extra runtime DLLs

add_library(darkestdiablo SHARED darkestdiablo_plugin.cpp darkestdiablo_plugin.rc)
target_link_libraries(darkestdiablo PRIVATE D2RLPlugin::D2RLPlugin)
set_target_properties(darkestdiablo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/../../build/plugin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/../../build/plugin
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../../build/plugin)
