$log = 'd:\MyProjects\ESP32-C5\wifi_station\build_err.log'
if (Test-Path $log) {
    $fi = Get-Item $log
    Write-Host ('SIZE=' + $fi.Length + ' TIME=' + $fi.LastWriteTime)
    Get-Content $log -Tail 20
} else {
    Write-Host 'NO_LOG'
}
Write-Host '---PROCS---'
Get-Process -Name ninja, gcc, cc1, python, 'cc1plus' -ErrorAction SilentlyContinue |
    Select-Object Name, Id, CPU, @{n='WS_MB';e={[math]::Round($_.WS/1MB,1)}} |
    Format-Table -AutoSize
Write-Host '---BUILD DIR---'
if (Test-Path 'd:\MyProjects\ESP32-C5\wifi_station\build') {
    Get-ChildItem 'd:\MyProjects\ESP32-C5\wifi_station\build\log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 6 Name, Length, LastWriteTime
} else {
    Write-Host 'NO_BUILD_DIR'
}