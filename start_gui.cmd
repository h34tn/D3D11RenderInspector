@echo off
setlocal
cd /d "%~dp0"
python -c "import sys, tkinter; assert sys.version_info >= (3, 10) and sys.maxsize > 2**32" >nul 2>&1
if errorlevel 1 (
 echo Python 3.10+ x64 with Tcl/Tk is required. Install Python from python.org with Tcl/Tk enabled.
 pause
 exit /b 1
)
python -c "import pathlib, subprocess, sys; subprocess.Popen([str(pathlib.Path(sys.executable).with_name('pythonw.exe')), str(pathlib.Path('inspector.pyw').resolve())], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)"
if errorlevel 1 (
 pause
 exit /b 1
)
