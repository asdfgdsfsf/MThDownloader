@echo off
rem Uninstall: remove the Chrome/Edge native messaging host registrations.
setlocal

for %%B in (
  "Google\Chrome"
  "Google\Chrome Beta"
  "Google\Chrome Dev"
  "Microsoft\Edge"
  "Microsoft\Edge Beta"
  "Microsoft\Edge Dev"
) do (
  reg delete "HKCU\Software\%%~B\NativeMessagingHosts\com.mthdownloader.bridge" /f >nul 2>&1
)

echo Removed Chrome/Edge native messaging host registrations.
exit /b 0
