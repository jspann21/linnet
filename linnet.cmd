@echo off
if exist "%~dp0.venv\Scripts\python.exe" (
    "%~dp0.venv\Scripts\python.exe" "%~dp0linnet" %*
) else (
    python "%~dp0linnet" %*
)
