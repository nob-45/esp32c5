import os
import sys
print("Python executable:", sys.executable)
print("ESP_ROM_ELF_DIR:", os.environ.get("ESP_ROM_ELF_DIR", "<NOT SET>"))
print("IDF_PATH:", os.environ.get("IDF_PATH", "<NOT SET>"))