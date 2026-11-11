@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ======================================================================
echo    DualSense BB Controller Audio & HD Haptics Fix - Installer
echo    《死亡搁浅：导演剪辑版》DualSense BB 语音与 HD 触觉修复补丁 - 一键安装
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
echo Tip: You can also right-click this .bat file, edit CUSTOM_GAME_DIR, and save.
echo 提示：您也可以右键用记事本编辑此脚本，在 CUSTOM_GAME_DIR 等号后填入路径保存。
echo.
set /p "USER_INPUT=Please drag & drop or enter game folder path (请输入或直接拖入游戏目录): "
if "!USER_INPUT!"=="" (
    echo [Error / 错误] No path provided. Aborting. (未输入路径，安装中止。)
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
    echo                 检测到《死亡搁浅》正在运行，请先退出游戏后再安装！
    echo.
    pause
    exit /b 1
)

:: Copy DLL and INI files
if not exist "%~dp0XAudio2_9.dll" (
    echo [Error / 错误] 'XAudio2_9.dll' not found in current folder!
    pause
    exit /b 1
)

copy /Y "%~dp0XAudio2_9.dll" "!TARGET_DIR!\XAudio2_9.dll" >nul
if %errorlevel% neq 0 (
    echo [Error / 错误] Failed to copy XAudio2_9.dll. Please check folder permissions.
    pause
    exit /b 1
)

if not exist "!TARGET_DIR!\XAudio2_9_BBFix.ini" (
    if exist "%~dp0XAudio2_9_BBFix.ini" (
        copy /Y "%~dp0XAudio2_9_BBFix.ini" "!TARGET_DIR!\XAudio2_9_BBFix.ini" >nul
    )
)

echo ======================================================================
echo [Success / 安装成功]
echo DualSense BB Audio Fix deployed successfully to:
echo 补丁文件已成功部署至：
echo   - !TARGET_DIR!\XAudio2_9.dll
echo   - !TARGET_DIR!\XAudio2_9_BBFix.ini
echo.
echo Remember to disable Steam Input in Steam Game Properties before playing!
echo 请记得在 Steam 游戏属性中将控制器设为「禁用 Steam 输入」！
echo ======================================================================
echo.
pause
exit /b 0
