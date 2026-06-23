@echo off
setlocal

echo ============================================================
echo ESP-IDF build helper for this project
echo Project: %CD%
echo ESP-IDF: D:\Espressif\frameworks\esp-idf-v5.5.4
echo Python : D:\Espressif\python_env\idf5.5_py3.11_env
echo ============================================================
echo.

set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
set "ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011"
set "PYTHONNOUSERSITE=True"
set "PYTHONPATH="
set "PYTHONHOME="

set "PATH=D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.10;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;D:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20250422\openocd-esp32\bin;%PATH%"

echo Checking tools...
"D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" --version
cmake --version
ninja --version
riscv32-esp-elf-gcc --version
echo.

set "IDF_TARGET=esp32c5"

echo Cleaning build directory if exists...
if exist build rmdir /s /q build

echo Building project (target=%IDF_TARGET%)...
"D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" "D:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py" -DIDF_TARGET=esp32c5 build

set "BUILD_RESULT=%ERRORLEVEL%"
echo.
if "%BUILD_RESULT%"=="0" (
    echo Build finished successfully.
) else (
    echo Build failed. Error code: %BUILD_RESULT%
)

endlocal
exit /b %BUILD_RESULT%