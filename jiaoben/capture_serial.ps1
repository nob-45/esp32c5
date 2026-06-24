param(
    [string]$Port = "COM18",
    [int]$Seconds = 25
)
$p = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
$p.ReadTimeout = 1000
$p.Encoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
try {
    $p.Open()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $line = $p.ReadLine()
            Write-Output $line
        } catch {}
    }
} finally {
    $p.Close()
}