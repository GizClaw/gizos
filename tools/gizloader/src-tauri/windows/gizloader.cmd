@echo off
if "%~1"=="" (
  start "" "%~dp0gizloader.exe"
  exit /b 0
)
"%~dp0gizloader-cli.exe" %*
exit /b %errorlevel%
