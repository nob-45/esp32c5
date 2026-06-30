Remove-Item -Recurse -Force 'D:\MyProjects\ESP32-C5\wifi_station\build' -ErrorAction SilentlyContinue
Write-Host "BUILD_DIR_REMOVED"
Get-ChildItem 'D:\MyProjects\ESP32-C5\wifi_station\' -Filter 'build*' -ErrorAction SilentlyContinue | Select-Object Name, Length