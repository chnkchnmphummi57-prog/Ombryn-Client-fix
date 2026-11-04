@echo off
setlocal
cd /d "%~dp0"
echo === Ombryn build ===

set "VSW=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSW%" goto novs
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSW%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

if exist minhook\include\MinHook.h goto havemh
echo Downloading MinHook...
powershell -NoProfile -Command "Invoke-WebRequest https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.zip -OutFile minhook.zip; Expand-Archive minhook.zip -DestinationPath . -Force; Rename-Item minhook-1.3.3 minhook; Remove-Item minhook.zip"
if not exist minhook\include\MinHook.h goto dlfail
:havemh

if exist imgui\imgui.h goto haveimgui
echo Downloading Dear ImGui...
powershell -NoProfile -Command "Invoke-WebRequest https://github.com/ocornut/imgui/archive/refs/tags/v1.91.5.zip -OutFile imgui.zip; Expand-Archive imgui.zip -DestinationPath . -Force; Rename-Item imgui-1.91.5 imgui; Remove-Item imgui.zip"
if not exist imgui\imgui.h goto dlfail
:haveimgui

echo Building ombryn.dll ...
cl /nologo /LD /O2 /EHsc /std:c++17 /I minhook\include /I imgui culling.cpp minhook\src\buffer.c minhook\src\hook.c minhook\src\trampoline.c minhook\src\hde\hde64.c imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp imgui\backends\imgui_impl_win32.cpp imgui\backends\imgui_impl_dx11.cpp imgui\backends\imgui_impl_dx12.cpp user32.lib d3d11.lib d3d12.lib dxgi.lib /Fe:ombryn.dll
if errorlevel 1 goto fail

echo Building injector.exe ...
cl /nologo /O2 /EHsc /std:c++17 injector.cpp user32.lib gdi32.lib comdlg32.lib shell32.lib /Fe:injector.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 goto fail

echo.
echo DONE. ombryn.dll and injector.exe are in this folder.
pause
exit /b 0

:novs
echo.
echo Visual Studio C++ build tools were not found.
echo Install "Build Tools for Visual Studio" from Microsoft ^(free^), tick
echo "Desktop development with C++", then run this file again.
pause
exit /b 1

:dlfail
echo.
echo Download failed. Check your internet, then run this file again.
pause
exit /b 1

:fail
echo.
echo BUILD FAILED. Scroll up, copy the red error lines, and send them to Claude.
pause
exit /b 1
