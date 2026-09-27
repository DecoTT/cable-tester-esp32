@echo off
REM Lanzador rápido del flasher GUI
REM Ejecutar desde cualquier carpeta

cd /d "%~dp0"

REM Verificar Python
python --version >nul 2>&1
if errorlevel 1 (
    echo Python no encontrado. Instala Python 3.8+ desde python.org
    pause
    exit /b 1
)

REM Instalar dependencias si faltan
python -c "import esptool, serial" >nul 2>&1
if errorlevel 1 (
    echo Instalando dependencias...
    python -m pip install -r requirements.txt
)

python flasher.py
