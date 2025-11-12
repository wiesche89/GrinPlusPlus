@echo off
setlocal EnableExtensions

echo === GrinPlusPlus Release Build (VS2017 /MT) ===

rem ----- Paths -----
set "GPP_ROOT=C:\ProjekteGit\opensource\wiesche89\GrinPlusPlus"
set "VCPKG_ROOT=C:\ProjekteGit\opensource\vcpkg"
set "OVERLAY_TRIPLETS=%GPP_ROOT%\vcpkg\custom_triplets"
set "OVERLAY_PORTS=%GPP_ROOT%\vcpkg\custom_ports"
set "TRIPLET=x64-windows-static"

rem ----- Compiler options (without /WX-) -----
rem /MP  -> Multi-proc compile (per-file)
rem /EHsc, /utf-8, disable warning 4828 as before
set "CL=/MP /EHsc /utf-8 /wd4828"

rem ----- vcpkg Binary Caching -----
rem Build once, then use cache — saves a lot of time on rebuilds
set "VCPKG_FEATURE_FLAGS=binarycaching"
rem Choose a persistent cache path (not inside the repo)
if not defined VCPKG_DEFAULT_BINARY_CACHE set "VCPKG_DEFAULT_BINARY_CACHE=C:\vcpkg\_cache"

rem ----- Prepare build folder -----
if not exist "%GPP_ROOT%\build" (
  echo Creating build folder: "%GPP_ROOT%\build"
  mkdir "%GPP_ROOT%\build"
)
pushd "%GPP_ROOT%\build"

rem ----- CMake configure (VS2017) -----
rem IMPORTANT: For VS2017 use the Win64 generator, NO -A!
rem Optional: For faster dev links use RelWithDebInfo + FASTLINK:
rem   -DCMAKE_BUILD_TYPE=RelWithDebInfo
rem   and in the MSBuild call: -- /p:DebugType=FastLink
echo Configuring CMake (Release)...
cmake -G "Visual Studio 15 2017 Win64" ^
  -D CMAKE_BUILD_TYPE=Release ^
  -D CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
  -D VCPKG_TARGET_TRIPLET=%TRIPLET% ^
  -D CMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -D VCPKG_OVERLAY_TRIPLETS=%OVERLAY_TRIPLETS% ^
  -D VCPKG_OVERLAY_PORTS=%OVERLAY_PORTS% ^
  "%GPP_ROOT%"
if errorlevel 1 (
  echo CMake configure failed.
  popd
  endlocal
  exit /b 1
)

rem ----- Start build (max parallelism) -----
rem --parallel = run MSBuild projects in parallel
rem -- /m      = MSBuild uses multiple processes for projects/targets
rem /MP        = cl.exe compiles files in parallel (see CL above)
echo Building (Release)...
cmake --build . --config Release --parallel %NUMBER_OF_PROCESSORS% -- /m
rem Optional for faster dev iteration:
rem cmake --build . --config RelWithDebInfo --parallel %NUMBER_OF_PROCESSORS% -- /m /p:DebugType=FastLink

if errorlevel 1 (
  echo Build failed.
  popd
  endlocal
  exit /b 1
)

echo.
echo Release build completed successfully.
popd
endlocal
