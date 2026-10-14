@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio with the C++ workload was not found.
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist out mkdir out
cl /nologo /O2 /EHsc /std:c++20 /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Fo:out\ /Fe:out\bench.exe bench.cpp ^
    d3d11.lib dxgi.lib ole32.lib user32.lib pdh.lib mmdevapi.lib || exit /b 1
