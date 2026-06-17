if (WIN32)
    add_definitions(-DNOMINMAX)
    message("NOMINMAX")

    if (MSVC AND ENABLE_SEH_HANDLER)
        # Replace SEH flag if there is one
        string(REGEX REPLACE "\/(EH).+" "\/EHa" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa")
    endif ()
endif ()

# Many of our code are optimized to utilize offsetof.
# But it complains a lot with non-POD type, that's not what we want, so disable the warn flag.
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "Minimum OS X version to target for deployment" FORCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-invalid-offsetof")
endif ()

if (EMSCRIPTEN)
    # SDL2 headers need -sUSE_SDL=2 as a compile flag to get the real headers
    add_compile_options("SHELL:-sUSE_SDL=2")
    # pthreads: set BEFORE add_subdirectory so ALL translation units get atomics/bulk-memory
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
endif()

add_subdirectory(bridge)
add_subdirectory(common)
add_subdirectory(config)
add_subdirectory(cpu)
add_subdirectory(disasm)
add_subdirectory(dispatch)
add_subdirectory(drivers)
add_subdirectory(gdbstub)
add_subdirectory(j2me)
add_subdirectory(kernel)
add_subdirectory(ldd)
add_subdirectory(loader)
add_subdirectory(mem)
add_subdirectory(package)
add_subdirectory(services)
add_subdirectory(system)
add_subdirectory(utils)
add_subdirectory(vfs)

# The desktop frontend needs Qt. A host build that only wants the core libraries
# (e.g. the dyncom differential test harness) has no Qt and no use for a UI, so
# allow skipping the frontend. Default ON -> normal builds are unchanged.
option(EKA2L1_BUILD_FRONTEND "Build the platform frontend (Qt desktop UI on host)" ON)

if (ANDROID)
    include_directories(android/app/src/main)
    add_subdirectory(android/app/src/main/cpp)
elseif(EMSCRIPTEN)
    add_subdirectory(web)
elseif(EKA2L1_OHOS)
    # HarmonyOS native frontend (static lib). The NAPI entry module in
    # hos/entry/src/main/cpp links it and DevEco/hvigor packages the result.
    add_subdirectory(hos/native)
elseif(EKA2L1_BUILD_FRONTEND)
    add_subdirectory(qt)
endif()

if (EKA2L1_ENABLE_SCRIPTING_ABILITY)
    add_subdirectory(scripting)
endif ()
