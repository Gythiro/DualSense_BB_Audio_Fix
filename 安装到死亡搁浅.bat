@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "GAME_DIR=D:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"

if not exist "%GAME_DIR%\ds.exe" (
    echo [提示] 未在默认路径找到 ds.exe:
    echo        %GAME_DIR%
    set /p "GAME_DIR=请输入《死亡搁浅：导演剪辑版》安装目录路径: "
)

if not exist "%GAME_DIR%\ds.exe" (
    echo [错误] 该目录下未找到 ds.exe，安装中止。
    pause
    exit /b 1
)

tasklist /FI "IMAGENAME eq ds.exe" 2>nul | find /I "ds.exe" >nul
if %errorlevel% equ 0 (
    echo [错误] 检测到游戏正在运行，请先退出《死亡搁浅》后再安装！
    pause
    exit /b 1
)

copy /Y "XAudio2_9.dll" "%GAME_DIR%\XAudio2_9.dll" >nul
if not exist "%GAME_DIR%\XAudio2_9_BBFix.ini" (
    copy /Y "XAudio2_9_BBFix.ini" "%GAME_DIR%\XAudio2_9_BBFix.ini" >nul
)

echo ========================================================
echo [安装成功] 已将 DualSense BB Controller Audio Fix 部署至：
echo   %GAME_DIR%\XAudio2_9.dll
echo   %GAME_DIR%\XAudio2_9_BBFix.ini
echo ========================================================
pause
