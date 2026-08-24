@echo off
setlocal

set ROOT=%~dp0
set OUT=%ROOT%build
set EXE=%OUT%\ImmersiveTopTaskbar.exe

if not exist "%OUT%" mkdir "%OUT%"

where cl >nul 2>nul
if %errorlevel% neq 0 (
  if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)

where cl >nul 2>nul
if %errorlevel% neq 0 (
  set VSWHERE=C:\Progra~2\Microsoft Visual Studio\Installer\vswhere.exe
  if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%I
  )
  if defined VSINSTALL (
    call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)

where cl >nul 2>nul
if %errorlevel% neq 0 (
  echo MSVC x64 was not found. ExplorerTAP requires the Microsoft C++ ABI.
  echo Install Visual Studio 2022 Build Tools with "Desktop development with C++".
  exit /b 1
)

rem Compile the icon/version resource into a .res file.
rc /nologo /fo "%OUT%\app.res" "%ROOT%src\app.rc"
if %errorlevel% neq 0 (
  echo rc.exe failed to compile resources.
  exit /b 1
)

cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 /DUNICODE /D_UNICODE ^
  /Fo"%OUT%\\" /Fe"%EXE%" ^
  "%ROOT%src\main.cpp" ^
  user32.lib gdi32.lib shell32.lib dwmapi.lib ole32.lib oleaut32.lib advapi32.lib winhttp.lib comctl32.lib gdiplus.lib ^
  /link /SUBSYSTEM:WINDOWS "%OUT%\app.res"

exit /b %errorlevel%
