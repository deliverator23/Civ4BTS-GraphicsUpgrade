@echo off
rem Builds GraphicsUpgrade (the 32-bit proxy d3d9.dll) with the newest Visual Studio found by vswhere.
rem Output: build\d3d9.dll and build\GraphicsUpgrade.ini. For the release package: uv run tools/package.py

setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

if not exist build\shaders mkdir build\shaders

rem The games check d3d9.dll's file version at startup, so the proxy carries the system d3d9.dll's version numbers.
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_version_rc.ps1 || exit /b 1
rc /nologo /fo build\version.res build\version.rc || exit /b 1

rem Shaders, compiled into headers the DLL embeds.
set "FXC=fxc /nologo /O3"
%FXC% /T vs_1_1 /E VSMain /Vn g_waterVS /Fh build\shaders\water_vs.h shaders\water.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_waterPS /Fh build\shaders\water_ps.h shaders\water.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_waterShadowVS /Fh build\shaders\water_shadow_vs.h shaders\water_shadow.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_waterShadowPS /Fh build\shaders\water_shadow_ps.h shaders\water_shadow.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_terrainShadowVS /Fh build\shaders\terrain_shadow_vs.h shaders\terrain_shadow.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_terrainShadowPS /Fh build\shaders\terrain_shadow_ps.h shaders\terrain_shadow.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSFlood /Vn g_riverFloodVS /Fh build\shaders\river_flood_vs.h shaders\river_shadow.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSFlood /Vn g_riverFloodPS /Fh build\shaders\river_flood_ps.h shaders\river_shadow.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSRiver /Vn g_riverVS /Fh build\shaders\river_vs.h shaders\river_shadow.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSRiver /Vn g_riverPS /Fh build\shaders\river_ps.h shaders\river_shadow.hlsl >nul || exit /b 1
%FXC% /T ps_2_0 /E PSMain /Vn g_shadowBlurPS /Fh build\shaders\shadow_blur_ps.h shaders\shadow_blur.hlsl >nul || exit /b 1
%FXC% /T vs_2_0 /E VSMain /Vn g_treeLitVS /Fh build\shaders\tree_lit_vs.h shaders\tree_lit.hlsl >nul || exit /b 1

cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /Isrc /Ibuild\shaders ^
   /Fobuild\ /Fdbuild\ src\*.cpp ^
   /LD /Fe:build\d3d9.dll ^
   /link /DEF:src\d3d9.def /MACHINE:X86 user32.lib shell32.lib build\version.res || exit /b 1
copy /y GraphicsUpgrade.ini build\GraphicsUpgrade.ini >nul
echo Built build\d3d9.dll
