@echo off
:: --- Settings ---
set PROJECT_NAME=svl
set BACKUP_DEST=E:\git\svl
set TIMESTAMP=%DATE:~10,4%-%DATE:~4,2%-%DATE:~7,2%_%TIME:~0,2%%TIME:~3,2%
set TIMESTAMP=%TIMESTAMP: =0%

:: --- Ensure backup folder exists ---
if not exist "%BACKUP_DEST%" mkdir "%BACKUP_DEST%"

echo Backing up %PROJECT_NAME% to %BACKUP_DEST%...

:: --- Create the Bundle ---
:: This packages all branches and history into one file
git bundle create "%BACKUP_DEST%\%PROJECT_NAME%_%TIMESTAMP%.bundle" --all

echo.
echo Backup Complete: %PROJECT_NAME%_%TIMESTAMP%.bundle
pause
