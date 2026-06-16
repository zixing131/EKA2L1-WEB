# NAPI entry module for the EKA2L1 HarmonyOS app.
#
# DevEco/hvigor invokes CMake here (entry/src/main/cpp). We pull in the whole
# EKA2L1 tree from the repo root so the emulator core + the hos_frontend static
# library build with the OHOS toolchain, then link them into libentry.so.

cmake_minimum_required(VERSION 3.5.0)
project(eka2l1_hos)

# The EKA2L1 core is C++20; match it so this module's TUs see the same stdlib
# features (std::optional, is_enum_v, ...) the shared headers rely on.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

# Repo root is six levels up: src/emu/hos/entry/src/main/cpp -> repo root.
get_filename_component(EKA2L1_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)

# Build only what the app needs from the EKA2L1 tree.
set(EKA2L1_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(EKA2L1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EKA2L1_BUILD_VULKAN_BACKEND OFF CACHE BOOL "" FORCE)
set(EKA2L1_ENABLE_SCRIPTING_ABILITY OFF CACHE BOOL "" FORCE)

# Adds the core libraries plus src/emu/hos/native (hos_frontend) because the
# root detects CMAKE_SYSTEM_NAME=OHOS and selects the HarmonyOS frontend.
add_subdirectory(${EKA2L1_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/eka2l1)

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(entry SHARED
        napi_init.cpp
        xcomponent_bridge.cpp)

target_link_libraries(entry PUBLIC
        hos_frontend
        libace_napi.z.so
        libace_ndk.z.so
        libnative_window.so
        libEGL.so
        libGLESv3.so
        libhilog_ndk.z.so)
