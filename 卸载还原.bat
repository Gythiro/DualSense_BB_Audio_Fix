@echo off
chcp 65001 >nul
setlocal

set "GAME_DIR=D:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"

if not exist "%GAME_DIR%\ds.exe" (
    set /p "GAME_DIR=请输入《死亡搁浅：导演剪辑版》安装目录路径: "
)

tasklist /FI "IMAGENAME eq ds.exe" 2>nul | find /I "ds.exe" >nul
if %errorlevel% equ 0 (
    echo [错误] 检测到游戏正在运行，请先退出《死亡搁浅》后再卸载！
    pause
    exit /b 1
)

if exist "%GAME_DIR%\XAudio2_9.dll" del /F /Q "%GAME_DIR%\XAudio2_9.dll"
if exist "%GAME_DIR%\XAudio2_9_BBFix.ini" del /F /Q "%GAME_DIR%\XAudio2_9_BBFix.ini"
if exist "%GAME_DIR%\XAudio2_9_BBFix.log" del /F /Q "%GAME_DIR%\XAudio2_9_BBFix.log"

echo [卸载完成] 已从游戏目录移除补丁文件，恢复原版状态。
pause
