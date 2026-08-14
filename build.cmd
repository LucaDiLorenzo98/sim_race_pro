@echo off
:: SimRacePro Build Script
:: Usage: build.cmd [Debug|Release]   (default: Debug)
setlocal

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug

:: Find vcvars64.bat via vswhere (works for Community/Professional/Enterprise)
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist %VSWHERE% (
    echo ERROR: vswhere.exe not found. Is Visual Studio 2022 installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (
    `%VSWHERE% -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find VC\Auxiliary\Build\vcvars64.bat`
) do set VCVARS=%%i

if "%VCVARS%"=="" (
    echo ERROR: Could not find vcvars64.bat
    exit /b 1
)

call "%VCVARS%" >nul 2>&1

msbuild SimRacePro.sln /p:Configuration=%CONFIG% /p:Platform=x64 /m /nologo /v:minimal
exit /b %ERRORLEVEL%
