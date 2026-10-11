@echo off
rem Builds the test harness (32-bit): build\test\test_proxy.exe, with its test shaders compiled into build\test\shaders.
rem Needs build.bat to have run first (the harness also uses GraphicsUpgrade's own water shader).
rem The tests themselves: uv run tests/run_tests.py

setlocal
cd /d "%~dp0.."

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

if not exist build\test\shaders mkdir build\test\shaders
set "FXC=fxc /nologo"
set "T=tests\shaders"
set "O=build\test\shaders"
%FXC% /T vs_2_0 /E VSMain /Vn g_testVS /Fh %O%\test_vs.h %T%\test_shaders.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_testPS /Fh %O%\test_ps.h %T%\test_shaders.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_ghostVS /Fh %O%\test_ghost_vs.h %T%\test_ghost.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_litVS /Fh %O%\test_lit_vs.h %T%\test_lit.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_testWaterVS /Fh %O%\test_water_vs.h %T%\test_water.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_btsWaterVS /Fh %O%\test_btswater_vs.h %T%\test_btswater.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_btsWaterPS /Fh %O%\test_btswater_ps.h %T%\test_btswater.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_terrainVS /Fh %O%\test_terrain_vs.h %T%\test_terrain.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_terrainPS /Fh %O%\test_terrain_ps.h %T%\test_terrain.hlsl >nul || exit /b 1
rem The same, named as older (non-Steam) BtS builds name them
%FXC% /D OLD_NAMES /T vs_2_0 /E VSMain /Vn g_ghostOldVS /Fh %O%\test_ghost_old_vs.h %T%\test_ghost.hlsl >nul || exit /b 1
%FXC% /D OLD_NAMES /T ps_2_0 /E PSMain /Vn g_terrainOldPS /Fh %O%\test_terrain_old_ps.h %T%\test_terrain.hlsl >nul || exit /b 1

cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Ibuild\test\shaders /Ibuild\shaders ^
   /Fobuild\test\ /Fdbuild\test\ tests\test_proxy.cpp ^
   /Fe:build\test\test_proxy.exe /link user32.lib || exit /b 1
echo Built build\test\test_proxy.exe
