@echo off
setlocal
cd /d "%~dp0"
if not exist build\generated\exports.inc (
 echo Run build.cmd first.
 exit /b 1
)
if not exist build\detours-source\src\detours.h (
 echo Fetch Microsoft Detours v4.0.1 into build\detours-source first. See README.
 exit /b 1
)
for /f %%i in ('git -C build\detours-source rev-parse HEAD') do set "INSPECTOR_DETOURS_REV=%%i"
if not "%INSPECTOR_DETOURS_REV%"=="e4bfd6b03e50de46b47abfbd1e46b384f0c5f833" (
 echo Detours source revision mismatch.
 exit /b 1
)
git -C build\detours-source diff --quiet HEAD
if errorlevel 1 (
 echo Detours tracked sources were modified; refusing to build.
 exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "INSPECTOR_VS=%%i"
if not defined INSPECTOR_VS exit /b 1
call "%INSPECTOR_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
pushd build\detours-source\src
nmake /nologo
if errorlevel 1 (
 popd
 exit /b 1
)
popd
if not exist build\injected mkdir build\injected
copy /y build\detours-source\LICENSE.md build\injected\Detours.LICENSE.txt >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc /LD /DINSPECTOR_INJECTED /Ibuild\generated /Isrc /Ibuild\detours-source\include src\proxy.cpp src\observer.cpp src\config.cpp build\detours-source\lib.X64\detours.lib dxguid.lib /Fo"build\injected\\" /link /OUT:build\inspector_hook.dll /IMPLIB:build\injected\hook.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc src\startup_launcher.cpp /Fo"build\injected\\" /Fe:build\startup_launcher.exe
exit /b %errorlevel%
