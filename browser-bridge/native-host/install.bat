@echo off
rem One-click install: build the extension (dist/chrome, dist/edge) and register
rem the Chrome/Edge native messaging host for the current user.
setlocal
cd /d "%~dp0"

echo [1/3] Building extension and host manifest ...
node "%~dp0..\build.js"
if errorlevel 1 (
  if exist "%~dp0com.mthdownloader.bridge.json" (
    echo Node.js not found - using prebuilt manifest and dist.
  ) else (
    echo Build failed. Please install Node.js or run: npm run build
    exit /b 1
  )
)

if not exist "%~dp0com.mthdownloader.bridge.json" (
  echo Missing com.mthdownloader.bridge.json - build was incomplete.
  exit /b 1
)

if not exist "%~dp0mth_bridge_host.exe" (
  echo Missing mth_bridge_host.exe - native messaging host is required.
  exit /b 1
)

echo [2/3] Registering Chrome native messaging host ...
reg add "HKCU\Software\Google\Chrome\NativeMessagingHosts\com.mthdownloader.bridge" /ve /t REG_SZ /d "%~dp0com.mthdownloader.bridge.json" /f >nul
if errorlevel 1 goto :regfail

echo [3/3] Registering Edge native messaging host ...
reg add "HKCU\Software\Microsoft\Edge\NativeMessagingHosts\com.mthdownloader.bridge" /ve /t REG_SZ /d "%~dp0com.mthdownloader.bridge.json" /f >nul
if errorlevel 1 goto :regfail

rem Optional beta/dev channels (non-fatal)
reg add "HKCU\Software\Google\Chrome Beta\NativeMessagingHosts\com.mthdownloader.bridge" /ve /t REG_SZ /d "%~dp0com.mthdownloader.bridge.json" /f >nul 2>&1
reg add "HKCU\Software\Google\Chrome Dev\NativeMessagingHosts\com.mthdownloader.bridge" /ve /t REG_SZ /d "%~dp0com.mthdownloader.bridge.json" /f >nul 2>&1
reg add "HKCU\Software\Microsoft\Edge Beta\NativeMessagingHosts\com.mthdownloader.bridge" /ve /t REG_SZ /d "%~dp0com.mthdownloader.bridge.json" /f >nul 2>&1
reg add "HKCU\Software\Microsoft\Edge Dev\NativeMessagingHosts\com.mthdownloader.bridge" /ve /t REG_SZ /d "%~dp0com.mthdownloader.bridge.json" /f >nul 2>&1

echo.
echo Done.
echo 1) Edit native-host\bridge_config.json to confirm the MThDownloader.exe path and arguments.
echo 2) Chrome: open chrome://extensions - Developer mode - Load unpacked - choose dist\chrome
echo 3) Edge  : open edge://extensions - Developer mode - Load unpacked - choose dist\edge
echo 4) Click the extension icon and press "Test bridge".
exit /b 0

:regfail
echo Failed to write registry. Run this script as the current user (no admin needed).
exit /b 1
