$log1 = 'D:\MyProjects\ESP32-C5\wifi_station\build\log\idf_py_stderr_output_26864'
$log2 = 'D:\MyProjects\ESP32-C5\wifi_station\build\log\idf_py_stdout_output_26864'
foreach ($log in @($log1, $log2)) {
    Write-Host "================ $log ================"
    if (Test-Path $log) {
        $info = Get-Item $log
        Write-Host "SIZE=$($info.Length)"
        Get-Content $log -ErrorAction SilentlyContinue | Select-Object -Last 80
    } else {
        Write-Host "(not found)"
    }
}
# 顺便扫 build_err.log 中出现 'error' 或 'FATAL' 的行
$buildErr = 'd:\MyProjects\ESP32-C5\wifi_station\build_err.log'
if (Test-Path $buildErr) {
    Write-Host "================ build_err.log matches ================"
    $m = Get-Content $buildErr -ErrorAction SilentlyContinue | Select-String -Pattern 'error|FATAL|undefined|cannot|fail' -CaseSensitive:$false
    if ($m) { $m | Select-Object -First 60 } else { Write-Host "(no matches)" }
}
# 同时列出最近一次 build 目录里生成的 ninja 日志
$logDir = 'D:\MyProjects\ESP32-C5\wifi_station\build\log'
if (Test-Path $logDir) {
    Write-Host "================ last 10 logs in build\log ================"
    Get-ChildItem $logDir -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 10 | Select-Object Name, Length, LastWriteTime
}