# Builds tools/extract.cpp into extract.exe with CascLib (MIT) linked in, so it is one file.
# The launcher bundles it to read the game files the build needs. tools\build-extract.cmd runs this.
cmake_minimum_required(VERSION 3.24)
project(dd_extract LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no runtime DLLs needed
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # CascLib's CMakeLists asks for an old CMake

include(FetchContent)
set(CASC_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(CASC_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(CASC_UNICODE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CascLib
  GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
  GIT_TAG 6b889ec8f421cbc9baccec296c582362ab917887)
FetchContent_MakeAvailable(CascLib)

add_executable(extract ../extract.cpp)
target_link_libraries(extract PRIVATE casc_static)
target_compile_definitions(extract PRIVATE CASCLIB_NO_AUTO_LINK_LIBRARY)
target_compile_features(extract PRIVATE cxx_std_17)
set_target_properties(extract PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/../../build/extract
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/../../build/extract)
