@echo off
cd /d "%~dp0"
if exist ".venv\Scripts\pythonw.exe" (
    start "" ".venv\Scripts\pythonw.exe" -m tools.studio.media_entry
    if errorlevel 1 (
        pause
        exit /b 1
    )
    exit /b 0
)
if exist ".venv\Scripts\python.exe" (
    ".venv\Scripts\python.exe" -m tools.studio.media_entry
) else (
    python -m tools.studio.media_entry
)
if errorlevel 1 pause
