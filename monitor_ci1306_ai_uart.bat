@echo off
setlocal
set PORT=%~1
set BAUD=%~2
if "%PORT%"=="" set PORT=COM7
if /I "%PORT%"=="list" goto list
if "%BAUD%"=="" set BAUD=921600
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor_ci1306_ai_uart.ps1" -Port "%PORT%" -Baud %BAUD%
exit /b %ERRORLEVEL%
:list
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0monitor_ci1306_ai_uart.ps1" -List
