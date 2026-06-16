@echo off
REM ============================================================================
REM EKA2L1-WEB HarmonyOS (HOS) build -- Windows port of build_hos.sh.
REM
REM Output: build_wasm_hos\  (full protection, local-origin allowed)
REM Like the Release build but with EKA2L1_HOS_BUILD=ON:
REM   - Copyright notice, file-integrity checks and version watermark are active.
REM   - Domain whitelist is relaxed to allow empty / local hosts so the wasm can
REM     run inside a HarmonyOS WebView (file://, resource://, custom scheme).
REM   - Channel watermark shows "HOS" instead of "Release".
REM The POST_BUILD step (gen_integrity.py) still seals the asset hashes.
REM
REM Usage:
REM   build_hos.bat              REM configure + build
REM   set JOBS=8 ^& build_hos.bat REM override parallelism (default 4)
REM
REM Requires the Emscripten SDK. Either run from an "emsdk activated" shell, or
REM set EMSDK to the emsdk root (this script will call emsdk_env.bat for you).
REM ============================================================================
setlocal enableextensions

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build_wasm_hos"
if not defined JOBS set "JOBS=4"

REM --- Locate emcmake (Emscripten) -------------------------------------------
where emcmake >nul 2>nul
if errorlevel 1 (
    if defined EMSDK if exist "%EMSDK%\emsdk_env.bat" (
        call "%EMSDK%\emsdk_env.bat"
    ) else (
        echo error: emcmake not found and EMSDK\emsdk_env.bat not located. Activate emsdk first. 1>&2
        exit /b 1
    )
)

where emcmake >nul 2>nul
if errorlevel 1 (
    echo error: emcmake still not on PATH after sourcing emsdk_env.bat. 1>&2
    exit /b 1
)

REM --- Configure --------------------------------------------------------------
call emcmake cmake -S "%ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
    -DEKA2L1_DEBUG_BUILD=OFF ^
    -DEKA2L1_HOS_BUILD=ON ^
    -DEKA2L1_BUILD_TESTS=OFF ^
    -DEKA2L1_BUILD_TOOLS=OFF ^
    -DEKA2L1_BUILD_PATCH=OFF ^
    -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF
if errorlevel 1 (
    echo error: CMake configure failed. 1>&2
    exit /b 1
)

REM --- Build ------------------------------------------------------------------
call cmake --build "%BUILD_DIR%" --target eka2l1_wasm -j%JOBS%
if errorlevel 1 (
    echo error: build failed. 1>&2
    exit /b 1
)

echo.
echo HOS build complete -^> %BUILD_DIR%\bin
echo Integrity sealed (gen_integrity.py); local origins (file://, resource://) are allowed.
endlocal
