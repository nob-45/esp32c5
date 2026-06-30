Get-CimInstance -ClassName Win32_PnPEntity |
    Where-Object { $_.Name -match '\(COM\d+\)' } |
    Select-Object -ExpandProperty Name