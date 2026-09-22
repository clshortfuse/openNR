@echo off
call "%~dp0build-tools.cmd"
if errorlevel 1 exit /b %errorlevel%
"%~dp0build\tools\Release\opennr_text_tests.exe"
set "result=%errorlevel%"
exit /b %result%