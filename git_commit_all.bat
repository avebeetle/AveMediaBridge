@echo off
chcp 65001 >nul
setlocal

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo ERROR: This is not a git repository.
    pause
    exit /b 1
)

for /f "delims=" %%b in ('git branch --show-current') do set "BRANCH=%%b"

if "%BRANCH%"=="" (
    echo ERROR: Detached HEAD. No branch selected.
    pause
    exit /b 1
)

echo.
echo Current branch: %BRANCH%

echo.
git status -sb

echo.
set /p "MSG=Commit message: "

if "%MSG%"=="" (
    echo ERROR: Empty commit message.
    pause
    exit /b 1
)

echo.
echo Adding all changes...
git add -A

git diff --cached --quiet
if not errorlevel 1 (
    echo.
    echo Nothing to commit.
    echo Trying to push existing local commits...
    goto PUSH
)

echo.
echo Committing...
git commit -m "%MSG%"

if errorlevel 1 (
    echo ERROR: Commit failed.
    pause
    exit /b 1
)

echo.
echo Last commit:
git log --oneline -1

:PUSH
echo.
echo Pushing branch %BRANCH%...

git rev-parse --abbrev-ref --symbolic-full-name @{u} >nul 2>&1
if errorlevel 1 (
    echo No upstream found. Setting upstream to origin/%BRANCH%...
    git push -u origin %BRANCH%
) else (
    git push
)

if errorlevel 1 (
    echo.
    echo ERROR: Push failed.
    pause
    exit /b 1
)

echo.
echo Done. Commit/push complete.

echo.
git status -sb

echo.
pause