@echo off
cmake -S "%~dp0." -B "%~dp0build\tools" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~dp0build\tools" --config Release
exit /b %errorlevel%