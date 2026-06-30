import os, sys
print("ESP_ROM_ELF_DIR:", os.environ.get("ESP_ROM_ELF_DIR", "<NOT SET>"))
print("IDF_PATH:", os.environ.get("IDF_PATH", "<NOT SET>"))
p = os.environ.get("ESP_ROM_ELF_DIR")
if p:
    print("exists:", os.path.exists(p))
    if os.path.exists(p):
        print("files:", os.listdir(p)[:10])