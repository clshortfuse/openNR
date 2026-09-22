@echo off
call "%~dp0openNR.cmd" build %*
exit /b %errorlevel%