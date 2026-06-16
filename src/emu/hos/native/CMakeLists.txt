# HarmonyOS native frontend for EKA2L1.
#
# Built as a static library that the NAPI entry module
# (hos/entry/src/main/cpp) links into libentry.so. Mirrors the Android
# native-lib target: same emulator core libraries, but with an OHOS window
# (XComponent + EGL) and the audio/camera/sensor null backends.

cmake_minimum_required(VERSION 3.8)

add_library(hos_frontend STATIC
        include/hos/emu_window_ohos.h
        include/hos/input_dialog.h
        include/hos/launcher.h
        include/hos/state.h
        include/hos/thread.h
        include/hos/ui_bridge.h
        src/emu_window_ohos.cpp
        src/input_dialog.cpp
        src/launcher.cpp
        src/platform_stubs.cpp
        src/state.cpp
        src/thread.cpp
        src/ui_bridge.cpp)

target_include_directories(hos_frontend PUBLIC include)

target_link_libraries(hos_frontend PUBLIC
        common
        cpu
        drivers
        epoc
        epockern
        epocpkg
        epocservs
        glad
        lunasvg
        stb
        sqlite3
        yaml-cpp)

# The Symbian DLL patches (avkonfep / goommonitor etc.) are copied into the
# device tree at runtime; make sure they are staged into bin/patch before the
# packaging step, exactly like the Android frontend does.
add_dependencies(hos_frontend
        audiooutputrouting
        avkonfep
        ecam
        scdv
        mediaclientaudio
        mediaclientaudiostream
        mediaclientvideo
        vibractrl
        backlightctrl
        postingsurfacefactory
        goommonitor)

# OpenGL ES via EGL is linked through the drivers library on OHOS.
