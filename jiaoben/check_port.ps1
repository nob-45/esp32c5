Get-CimInstance Win32_Process -Filter "Name = 'python.exe' OR Name = 'pythonw.exe'" |
    Where-Object { $_.CommandLine -ne $null } |
    Select-Object ProcessId, ParentProcessId, Name,
        @{Name='CmdLine'; Expression={ $_.CommandLine.Substring(0, [Math]::Min(180, $_.CommandLine.Length)) } } |
    Format-List