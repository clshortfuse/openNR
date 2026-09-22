@echo off
call "%~dp0build-tools.cmd"
if errorlevel 1 exit /b %errorlevel%
pushd "%~dp0"
"%~dp0build\tools\Release\opennr.exe" %*
set "result=%errorlevel%"
popd
exit /b %result%