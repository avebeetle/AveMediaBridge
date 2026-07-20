@echo off
chcp 65001 >nul
setlocal

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo ERROR: This is not a git repository.
    pause
    exit /b 1
)

echo.
echo Current branch:
git branch --show-current

echo.
echo Status:
git status -sb

echo.
echo Last commits:
git log --oneline -5

echo.
pause