@echo off
setlocal
cmake -S "%~dp0." -B "%~dp0..\..\build\benchmarks\native" -G Ninja -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~dp0..\..\build\benchmarks\native"
exit /b %errorlevel%