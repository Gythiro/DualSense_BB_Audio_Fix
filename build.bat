@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

where zig >nul 2>nul
if %errorlevel% equ 0 (
    set "ZIG_CMD=zig"
    goto :compile
)

if exist "tools\zig\zig.exe" (
    set "ZIG_CMD=tools\zig\zig.exe"
    goto :compile
)

if exist "%LOCALAPPDATA%\Programs\zig\zig.exe" (
    set "ZIG_CMD=%LOCALAPPDATA%\Programs\zig\zig.exe"
    goto :compile
)

echo ========================================================
echo [错误] 系统中未找到 zig 编译器 (zig.exe)
echo.
echo 请从官网下载 Zig 0.13.0 或更新版本:
echo   https://ziglang.org/download/
echo 解压后将 zig 所在目录添加至系统环境变量 PATH 即可。
echo ========================================================
pause
exit /b 1

:compile
echo 正在编译 64 位 XAudio2_9.dll ...
"%ZIG_CMD%" c++ -target x86_64-windows-gnu -shared -O2 -std=c++17 ^
    -fno-exceptions -fno-rtti -s ^
    src\xaudio2_proxy.cpp src\XAudio2_9.def ^
    -lole32 -lkernel32 -luser32 ^
    -o XAudio2_9.dll

if %errorlevel% neq 0 (
    echo.
    echo [失败] 编译出错，请检查源码或编译器版本！
    pause
    exit /b 1
)

if exist "xaudio2_proxy.lib" del /f /q "xaudio2_proxy.lib" >nul 2>nul

echo.
echo [成功] 已生成最新 XAudio2_9.dll
pause
