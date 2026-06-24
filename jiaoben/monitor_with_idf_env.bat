@echo off
setlocal

set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
set "ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011"
set "PYTHONNOUSERSITE=True"
set "PYTHONPATH="
set "PYTHONHOME="

set "PATH=D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.10;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;D:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20250422\openocd-esp32\bin;%PATH%"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM18"

echo Monitoring %PORT% ... (Ctrl+] to exit)
"D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" "D:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py" -p %PORT% monitor

endlocal & exit /b %ERRORLEVEL%