@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
 echo Visual Studio Installer / C++ Build Tools not found.
 exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "INSPECTOR_VS=%%i"
if not defined INSPECTOR_VS exit /b 1
call "%INSPECTOR_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist build\generated mkdir build\generated
python tools\generate_exports.py --output build\generated
if errorlevel 1 exit /b 1
ml64 /nologo /c /Fobuild\forward.obj build\generated\forward.asm
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc /LD /Ibuild\generated /Isrc src\proxy.cpp src\observer.cpp src\config.cpp build\forward.obj dxguid.lib /Fo"build\\" /link /DEF:build\generated\proxy.def /OUT:build\d3d11.dll /IMPLIB:build\proxy.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc /Isrc tests\render_smoke.cpp src\config.cpp /Fo"build\\" /Fe:build\render_smoke.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc tests\abi_test.cpp build\forward.obj /Fo"build\\" /Fe:build\abi_test.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc /Isrc tests\compute_smoke.cpp src\config.cpp /Fo"build\\" /Fe:build\compute_smoke.exe
if errorlevel 1 exit /b 1
python tools\generate_exports.py --output build\generated --verify build\d3d11.dll
exit /b %errorlevel%
