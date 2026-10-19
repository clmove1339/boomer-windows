@echo off
rem Run from "x64 Native Tools Command Prompt for VS", or let vcvars be found below.
where cl >nul 2>nul || call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W3 boomer.c user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib /link /SUBSYSTEM:WINDOWS
