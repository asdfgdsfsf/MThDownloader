@echo off
rem Chrome/Edge launches this wrapper with stdio. stdout must carry protocol
rem data only, so host logs are redirected to host.log.
setlocal
set "LOG=%~dp0host.log"

rem Prefer python, fall back to py -3 (Windows Python Launcher).
python "%~dp0mth_bridge_host.py" 2>>"%LOG%"
if not errorlevel 9009 exit /b %errorlevel%

py -3 "%~dp0mth_bridge_host.py" 2>>"%LOG%"
exit /b %errorlevel%
