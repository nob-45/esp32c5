$log = 'd:\MyProjects\ESP32-C5\wifi_station\build_err.log'
if (Test-Path $log) {
    $info = Get-Item $log
    Write-Host "LOG_SIZE=$($info.Length) BYTES"
    $lines = Get-Content $log -ErrorAction SilentlyContinue
    Write-Host "TOTAL_LINES=$($lines.Count)"
    Write-Host "---LAST 60 LINES---"
    $lines | Select-Object -Last 60
    Write-Host "---ERROR/STATUS MATCHES---"
    $matches = $lines | Select-String -Pattern 'error:|FAILED|fatal error|undefined reference|Build finished|Project size|Successfully|exit status|warning:'
    if ($matches) {
        $matches | Select-Object -First 50
    } else {
        Write-Host "(no matches)"
    }
} else {
    Write-Host "NO_LOG_FILE"
}