@echo off
cd /d d:\MyProjects\ESP32-C5\wifi_station
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
set "ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011"
set "PYTHONPATH="
set "PYTHONHOME="
set "PYTHONNOUSERSITE=True"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.12.1;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;%PATH%"
"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" build > d:\MyProjects\ESP32-C5\wifi_station\build_output.txt 2>&1
echo BUILD_EXIT_CODE=%ERRORLEVEL% >> d:\MyProjects\ESP32-C5\wifi_station\build_output.txt