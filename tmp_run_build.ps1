Set-Location 'd:\MyProjects\ESP32-C5\wifi_station'
$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.4'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\python_env\idf5.5_py3.11_env'
$env:ESP_ROM_ELF_DIR = 'D:\Espressif\tools\esp-rom-elfs\20241011'
$env:PYTHONNOUSERSITE = 'True'
$env:PYTHONPATH = ''
$env:PYTHONHOME = ''
$env:IDF_TARGET = 'esp32c5'
# 限制 ninja 并发 = 1 (避免 wpa_supplicant 巨型 .c OOM)
$env:NINJA_STATUS = '[%f/%t] '
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
$extra = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;D:\Espressif\tools\cmake\3.30.2\bin;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;'
$env:PATH = $extra + $env:PATH

$py = 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe'
$idfpy = 'D:\Espressif\frameworks\esp-idf-v5.5.4\tools\idf.py'
$log = 'd:\MyProjects\ESP32-C5\wifi_station\build_err.log'
Remove-Item $log -ErrorAction SilentlyContinue

& $py $idfpy -DIDF_TARGET=esp32c5 build 2>&1 | Tee-Object -FilePath $log
Write-Host '---DONE---'
Write-Host ("LASTEXITCODE=" + $LASTEXITCODE)