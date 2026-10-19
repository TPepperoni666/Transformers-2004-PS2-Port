@echo off
rem Run from a Visual Studio x64 developer shell. Build products stay outside sources.
if "%~1"=="" (echo Usage: build_game_fast.bat EXTERNAL_BUILD_FOLDER & exit /b 2)
start "" /b /belownormal /wait cmake --build "%~1" --target ps2EntryRunner --parallel 4
exit /b %ERRORLEVEL%
