@echo off
set /p BUNDLE_NAME="svl_2026-04-03_0951.bundle"
set /p DEST_FOLDER="C:\Shavais\Projects\C++\svl"

echo Restoring %BUNDLE_NAME% into %DEST_FOLDER%...

:: This clones the bundle just like it was a remote GitHub URL
git clone "%BUNDLE_NAME%" "%DEST_FOLDER%"

echo.
echo Restore complete. Your project is in %DEST_FOLDER%
pause