$log = 'd:\MyProjects\ESP32-C5\wifi_station\build_err.log'
if (-not (Test-Path $log)) { Write-Host "NO_LOG"; return }
$lines = Get-Content $log
# 找包含 error/fatal/FAILED/Error的任意行
$hits = for ($i=0; $i -lt $lines.Count; $i++) {
    $s = $lines[$i]
    if ($s -match '(?i)error[: ]|FAILED|fatal|undefined reference|warning:') {
        # 输出当前行 + 后5行上下文
        $start = [Math]::Max(0, $i-2)
        $end = [Math]::Min($lines.Count-1, $i+5)
        for ($j=$start; $j -le $end; $j++) {
            "$($j+1): $($lines[$j])"
        }
        "---"
    }
}
$hits | Select-Object -First 200