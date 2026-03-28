@echo off
echo [GhostsKor] Installing Python Dependencies...
pip install freetype-py Pillow

echo [GhostsKor] Generating Font Atlas 2 (PyeongChang)...
python tools/generate_atlas2.py

if %errorlevel% neq 0 (
    echo [GhostsKor] Error generating atlas2!
    pause
    exit /b %errorlevel%
)

echo [GhostsKor] Atlas2 Generation Complete.
