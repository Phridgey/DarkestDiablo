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

add_library(darkestdiablo SHARED darkestdiablo_plugin.cpp campfire.cpp devtools.cpp gamememory.cpp gamestate.cpp gameapi.cpp gamewrite.cpp zone.cpp andariel.cpp waveoverlay.cpp mercgear.cpp stash.cpp darkestdiablo_plugin.rc)
target_include_directories(darkestdiablo PRIVATE ${CMAKE_SOURCE_DIR}/../../build/plugin-gen)  # gametables.h (tools/gen_game_tables.py)
target_link_libraries(darkestdiablo PRIVATE D2RLPlugin::D2RLPlugin shell32 ole32 version)
set_target_properties(darkestdiablo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/../../build/plugin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/../../build/plugin
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../../build/plugin)

# The camp rules have no game code in them, so they are tested on their own. tools\build-plugin.cmd runs them.
add_executable(camp_tests ../../tests/camp_tests.cpp)
target_include_directories(camp_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_features(camp_tests PRIVATE cxx_std_20)
set_target_properties(camp_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/../../build/plugin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/../../build/plugin
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../../build/plugin)
