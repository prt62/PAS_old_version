@echo off
title Python TCP Test Installer

echo ==================================================
echo   AJEEVI PYTHON INSTALLER AND LAUNCHER
echo ==================================================
echo.

echo [STEP 1] Checking for Python 3.12 installation...
if not exist "%LocalAppData%\Programs\Python\Python312\python.exe" (
    echo [INFO] Python 3.12 not found.
    echo [INFO] Installing Python 3.12.2...
    
    start /wait python-3.12.2-amd64.exe /quiet InstallAllUsers=0 PrependPath=1 Include_pip=1

    echo [INFO] Python installer finished.
) else (
    echo [OK] Python 3.12 already installed.
)

echo.
echo [STEP 2] Setting Python path...

set PYTHON_EXE=%LocalAppData%\Programs\Python\Python312\python.exe

echo [INFO] Python executable:
echo %PYTHON_EXE%

if not exist "%PYTHON_EXE%" (
    echo [ERROR] Python executable not found!
    pause
    exit /b 1
)

echo.
echo [STEP 3] Checking virtual environment...

if not exist "venv" (
    echo [INFO] Creating virtual environment...
    "%PYTHON_EXE%" -m venv venv

    if errorlevel 1 (
        echo [ERROR] Failed to create virtual environment!
        pause
        exit /b 1
    )

    echo [OK] Virtual environment created.
) else (
    echo [OK] Virtual environment already exists.
)

echo.
echo [STEP 4] Activating virtual environment...

call venv\Scripts\activate.bat

if errorlevel 1 (
    echo [ERROR] Failed to activate virtual environment!
    pause
    exit /b 1
)

echo [OK] Virtual environment activated.

echo.
echo [STEP 5] Checking Python version...

python --version

echo.
echo [STEP 6] Upgrading pip...

python -m pip install --upgrade pip

if errorlevel 1 (
    echo [ERROR] Pip upgrade failed!
    pause
    exit /b 1
)

echo [OK] Pip upgraded.

echo.
echo [STEP 7] Installing requirements...

pip install -r requirements.txt

if errorlevel 1 (
    echo [ERROR] Requirements installation failed!
    pause
    exit /b 1
)

echo [OK] Requirements installed.

echo.
echo [STEP 8] Launching application...

if not exist "python_tcp_test.py" (
    echo [ERROR] python_tcp_test.py not found!
    pause
    exit /b 1
)

echo [INFO] Starting python_tcp_test.py...
echo.

python python_tcp_test.py

echo.
echo ==================================================
echo Application exited.
echo ==================================================

pause