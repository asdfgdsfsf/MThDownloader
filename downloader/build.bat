@echo off
rem ==== MThDownloader 构建脚本 ====
rem 源码在本目录, 编译产物直接输出到旁边的软件目录 (..\MThDownloader\)
rem 用法: 把 MINGW_BIN 改成你的 MinGW-w64 (posix 线程模型) bin 目录后运行
setlocal
set "HERE=%~dp0"
set "OUT=%HERE%..\MThDownloader"
if "%MINGW_BIN%"=="" set "MINGW_BIN=%HERE%..\tools\mingw64\bin"

if not exist "%OUT%" mkdir "%OUT%"

"%MINGW_BIN%\windres.exe" -I "%HERE%" "%HERE%res.rc" -O coff -o "%HERE%res.o"
if errorlevel 1 goto :err

"%MINGW_BIN%\g++.exe" -std=c++17 -O2 -municode -mwindows "%HERE%main.cpp" "%HERE%res.o" -o "%OUT%\MThDownloader.exe" -lwinhttp -lcomctl32 -lcomdlg32 -lshell32 -lole32 -static
if errorlevel 1 goto :err

del "%HERE%res.o"
echo 构建成功: %OUT%\MThDownloader.exe
exit /b 0
:err
echo 构建失败
exit /b 1
