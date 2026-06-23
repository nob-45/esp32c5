@echo off
setlocal

set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4"
set "IDF_TOOLS_PATH=D:\Espressif"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env"
set "ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011"
set "PYTHONNOUSERSITE=True"
set "PYTHONPATH="
set "PYTHONHOME="

set "PATH=D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\ccache\4.10;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;%PATH%"

set "IDF_TARGET=esp32c5"

echo Building (incremental, target=%IDF_TARGET%)...
"D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" "D:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py" -DIDF_TARGET=esp32c5 build > build_err.log 2>&1
set "BUILD_RESULT=%ERRORLEVEL%"

echo ===== ERROR LINES =====
findstr /I /C:"error:" /C:"FAILED" /C:"undefined reference" /C:"fatal error" build_err.log

echo.
if "%BUILD_RESULT%"=="0" (
    echo Build finished successfully.
) else (
    echo Build failed. Error code: %BUILD_RESULT%
)

endlocal
exit /b %BUILD_RESULT%