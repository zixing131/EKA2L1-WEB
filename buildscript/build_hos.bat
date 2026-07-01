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
REM   Double-click build_hos.bat         configure + build
REM   set JOBS=8 ^& build_hos.bat        override parallelism (default 4)
REM
REM Toolchain auto-bootstrap (nothing needs to be pre-activated):
REM   * CMake 3.x is REQUIRED: CMake 4.x hard-rejects vendored capstone's
REM     "cmake_policy(SET CMP0048 OLD)". The script prefers a 3.x already on
REM     PATH, else the pip-provided CMake (python -m pip install "cmake==3.31").
REM   * Emscripten: if emcmake is not on PATH, the sibling ..\emsdk is wired up
REM     (emsdk_env.bat first, then a manual PATH + .emscripten fallback for an
REM     emsdk that was downloaded but never "activate"d).
REM   * Ninja: must already be on PATH (e.g. "pip install ninja").
REM Set EMSDK to point at a non-default emsdk root if needed.
REM ============================================================================
setlocal enableextensions

REM --- Resolve repo root (this script lives in <root>\buildscript) ------------
for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "BUILD_DIR=%ROOT%\build_wasm_hos"
if not defined JOBS set "JOBS=4"

REM --- Locate the Emscripten SDK ----------------------------------------------
if not defined EMSDK if exist "%ROOT%\..\emsdk" set "EMSDK=%ROOT%\..\emsdk"
if not defined EMSDK (
    echo error: EMSDK not set and no sibling emsdk folder found next to the repo. 1>&2
    echo        Set EMSDK to your emsdk root, e.g. set "EMSDK=E:\path\to\emsdk" 1>&2
    exit /b 1
)
for %%I in ("%EMSDK%") do set "EMSDK=%%~fI"

call :ensure_cmake   || exit /b 1
call :ensure_emcmake || exit /b 1
call :ensure_ninja   || exit /b 1

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
exit /b 0


REM ============================================================================
REM Subroutines
REM ============================================================================

:ensure_emcmake
where emcmake >nul 2>nul && exit /b 0
REM Try the official activation route first.
set "EMSDK_QUIET=1"
if exist "%EMSDK%\emsdk_env.bat" call "%EMSDK%\emsdk_env.bat" >nul 2>nul
where emcmake >nul 2>nul && exit /b 0
REM Fallback: emsdk files present but never activated -> wire up PATH + config.
if not exist "%EMSDK%\upstream\emscripten\emcmake.exe" (
    echo error: emcmake not found under "%EMSDK%\upstream\emscripten". 1>&2
    echo        From "%EMSDK%" run: emsdk install latest ^&^& emsdk activate latest 1>&2
    exit /b 1
)
set "EM_NODE="
for /d %%N in ("%EMSDK%\node\*") do if exist "%%N\bin\node.exe" set "EM_NODE=%%N"
set "EM_PY="
for /d %%P in ("%EMSDK%\python\*") do if exist "%%P\python.exe" set "EM_PY=%%P"
set "PATH=%EMSDK%\upstream\emscripten;%EMSDK%\upstream\bin;%PATH%"
if defined EM_NODE set "PATH=%EM_NODE%\bin;%PATH%"
if defined EM_PY   set "PATH=%EM_PY%;%PATH%"
REM Minimal .emscripten config. Forward slashes avoid Python "\U" escape errors.
set "EM_CONFIG=%EMSDK%\.emscripten"
set "EMS=%EMSDK:\=/%"
set "NODE_LINE=NODE_JS = 'node'"
if defined EM_NODE set "NODE_LINE=NODE_JS = '%EM_NODE:\=/%/bin/node.exe'"
if not exist "%EM_CONFIG%" (
    > "%EM_CONFIG%" (
        echo %NODE_LINE%
        echo LLVM_ROOT = '%EMS%/upstream/bin'
        echo BINARYEN_ROOT = '%EMS%/upstream'
        echo EMSCRIPTEN_ROOT = '%EMS%/upstream/emscripten'
        echo TEMP_DIR = '%EMS%/tmp'
        echo COMPILER_ENGINE = NODE_JS
        echo JS_ENGINES = [NODE_JS]
    )
)
where emcmake >nul 2>nul && exit /b 0
echo error: emcmake still not on PATH after bootstrap. 1>&2
exit /b 1

:cmake_major
REM %1 = output variable name. Sets it to the major version of `cmake` on PATH
REM (e.g. 3 or 4), or "" if cmake is unavailable.
setlocal
set "V="
for /f "tokens=3" %%a in ('cmake --version 2^>nul ^| findstr /b /c:"cmake version"') do set "V=%%a"
set "M="
for /f "delims=." %%x in ("%V%") do set "M=%%x"
endlocal & set "%~1=%M%"
exit /b 0

:ensure_cmake
REM CMake 3.x is required. CMake 4.x rejects vendored capstone's CMP0048 OLD.
call :cmake_major CMAKE_MAJOR
if "%CMAKE_MAJOR%"=="3" exit /b 0
REM Prefer the pip-provided CMake (any 3.x) over a system 4.x.
set "PIP_CMAKE_BIN="
for /f "delims=" %%D in ('python -c "import cmake;print(cmake.CMAKE_BIN_DIR)" 2^>nul') do set "PIP_CMAKE_BIN=%%D"
if not defined PIP_CMAKE_BIN for %%V in (313 312 311 310 39) do if exist "%APPDATA%\Python\Python%%V\site-packages\cmake\data\bin\cmake.exe" set "PIP_CMAKE_BIN=%APPDATA%\Python\Python%%V\site-packages\cmake\data\bin"
if defined PIP_CMAKE_BIN set "PATH=%PIP_CMAKE_BIN%;%PATH%"
call :cmake_major CMAKE_MAJOR
if "%CMAKE_MAJOR%"=="3" exit /b 0
echo error: CMake 3.x is required; found major "%CMAKE_MAJOR%" on PATH. 1>&2
echo        CMake 4.x is incompatible with vendored capstone. Install 3.x, e.g.: 1>&2
echo        python -m pip install "cmake==3.31.10" 1>&2
exit /b 1

:ensure_ninja
where ninja >nul 2>nul && exit /b 0
echo error: ninja not found on PATH. Install it, e.g. "pip install ninja". 1>&2
exit /b 1
