@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ======================================================================
echo   DualSense BB Controller Audio & HD Haptics Fix - Uninstaller
echo   《死亡搁浅：导演剪辑版》DualSense BB 语音与 HD 触觉补丁 - 一键卸载还原
echo ======================================================================
echo.

:: ========================================================================
:: [User Configuration / 用户配置]
:: If your game is installed in a custom location, specify your path below:
:: 若游戏安装在自定义路径，请直接在下方等号后填入包含 ds.exe 的完整目录路径：
set "CUSTOM_GAME_DIR="
:: ========================================================================

set "TARGET_DIR="

:: 1. Check user-configured path
if defined CUSTOM_GAME_DIR (
    set "CLEAN_DIR=%CUSTOM_GAME_DIR:"=%"
    if exist "!CLEAN_DIR!\ds.exe" (
        set "TARGET_DIR=!CLEAN_DIR!"
        goto :FOUND_DIR
    )
)

:: 2. Check if script is already running inside the game directory
if exist "%~dp0ds.exe" (
    set "TARGET_DIR=%~dp0"
    if "!TARGET_DIR:~-1!"=="\" set "TARGET_DIR=!TARGET_DIR:~0,-1!"
    goto :FOUND_DIR
)

:: 3. Scan common Steam and Epic Games installation locations
for %%D in (
    "C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "D:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "E:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "F:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "G:\SteamLibrary\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "D:\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "E:\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT"
    "C:\Program Files\Epic Games\DeathStrandingDirector'sCut"
    "D:\Epic Games\DeathStrandingDirector'sCut"
    "E:\Epic Games\DeathStrandingDirector'sCut"
) do (
    if exist "%%~fD\ds.exe" (
        set "TARGET_DIR=%%~fD"
        goto :FOUND_DIR
    )
)

:: 4. Prompt user if auto-detection did not find ds.exe
:PROMPT_USER
echo [Notice / 提示] Could not automatically locate Death Stranding Director's Cut.
echo               未能自动定位到包含 ds.exe 的游戏安装根目录。
echo.
set /p "USER_INPUT=Please drag & drop or enter game folder path (请输入或直接拖入游戏目录): "
if "!USER_INPUT!"=="" (
    echo [Error / 错误] No path provided. Aborting. (未输入路径，卸载中止。)
    pause
    exit /b 1
)

set "CLEAN_DIR=!USER_INPUT:"=!"
if "!CLEAN_DIR:~-1!"=="\" set "CLEAN_DIR=!CLEAN_DIR:~0,-1!"

if not exist "!CLEAN_DIR!\ds.exe" (
    echo.
    echo [Error / 错误] 'ds.exe' not found in: "!CLEAN_DIR!"
    echo                该目录下未检测到 ds.exe，请确保路径正确。
    echo.
    goto :PROMPT_USER
)

set "TARGET_DIR=!CLEAN_DIR!"

:FOUND_DIR
echo [Target / 目标目录]: !TARGET_DIR!
echo.

:: Check if game is currently running
tasklist /FI "IMAGENAME eq ds.exe" 2>nul | find /I "ds.exe" >nul
if %errorlevel% equ 0 (
    echo [Warning / 警告] 'ds.exe' is currently running! Please close the game first.
    echo                 检测到《死亡搁浅》正在运行，请先退出游戏后再卸载！
    echo.
    pause
    exit /b 1
)

if exist "!TARGET_DIR!\XAudio2_9.dll" del /F /Q "!TARGET_DIR!\XAudio2_9.dll"
if exist "!TARGET_DIR!\XAudio2_9_BBFix.ini" del /F /Q "!TARGET_DIR!\XAudio2_9_BBFix.ini"
if exist "!TARGET_DIR!\XAudio2_9_BBFix.log" del /F /Q "!TARGET_DIR!\XAudio2_9_BBFix.log"

echo ======================================================================
echo [Success / 卸载完成]
echo Patch files removed from game directory. Restored to vanilla state.
echo 已从游戏目录完全移除补丁文件，游戏已恢复原版状态。
echo ======================================================================
echo.
pause
exit /b 0
