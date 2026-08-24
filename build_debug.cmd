@echo off
setlocal

set ROOT=%~dp0
set OUT=%ROOT%build
set EXE=%OUT%\ImmersiveTopTaskbar.debug.exe
set PDB=%OUT%\ImmersiveTopTaskbar.debug.pdb

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
  echo MSVC x64 was not found.
  exit /b 1
)

rc /nologo /fo "%OUT%\app.debug.res" "%ROOT%src\app.rc"
if %errorlevel% neq 0 exit /b %errorlevel%

cl /nologo /std:c++20 /EHsc /Od /Zi /W4 /utf-8 /DUNICODE /D_UNICODE /DDEBUG ^
  /Fo"%OUT%\debug_" /Fd"%PDB%" /Fe"%EXE%" ^
  "%ROOT%src\main.cpp" ^
  user32.lib gdi32.lib shell32.lib dwmapi.lib ole32.lib oleaut32.lib advapi32.lib winhttp.lib comctl32.lib gdiplus.lib ^
  /link /SUBSYSTEM:WINDOWS /DEBUG "%OUT%\app.debug.res"

exit /b %errorlevel%
