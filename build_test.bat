@echo off
cd /d "d:\MyProjects\ESP32-C5\wifi_station"
call "C:\Espressif\frameworks\esp-idf-v5.5\export.bat" >nul 2>&1
if errorlevel 1 (
    call "C:\Espressif\frameworks\esp-idf-v5.3\export.bat" >nul 2>&1
)
idf.py set-target esp32c5 >nul 2>&1
echo === BUILD START ===
idf.py build 2>&1
echo === BUILD END exit=%errorlevel% ===