@echo off
echo [GhostsKor] Installing Python Dependencies...
pip install freetype-py Pillow

echo [GhostsKor] Generating Font Atlas...
python tools/generate_atlas.py

if %errorlevel% neq 0 (
    echo [GhostsKor] Error generating atlas!
    pause
    exit /b %errorlevel%
)

echo [GhostsKor] Atlas Generation Complete.

