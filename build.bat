@echo off
setlocal

cd /d "%~dp0"

set ARCH=%1
if "%ARCH%"=="" set ARCH=x64

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found, is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build

cl /nologo /W3 /O2 /Zi /MT /std:clatest /D_CRT_SECURE_NO_WARNINGS ^
   /FIphnt_compat.h /Isrc /Ivendor\phnt /Ivendor\SlimDetours ^
   /Fobuild\ /Fdbuild\ /Febuild\repro.exe ^
   src\repro.c vendor\SlimDetours\*.c ^
   /link /DEBUG ntdll.lib dbghelp.lib
if errorlevel 1 exit /b 1

echo.
echo Built build\repro.exe (%ARCH%)
