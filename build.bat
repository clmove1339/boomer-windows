@echo off
rem Run from "x64 Native Tools Command Prompt for VS", or let vcvars be found below (any VS 2017+ edition).
where cl >nul 2>nul || for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W3 boomer.c user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib dwmapi.lib /link /SUBSYSTEM:WINDOWS
