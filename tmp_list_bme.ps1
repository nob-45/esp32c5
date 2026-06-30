$root = 'D:\MyProjects\esp-dev-kits-master\examples\esp-sensairshuttle'
if (-not (Test-Path $root)) { Write-Output "MISSING $root"; exit }
Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'bme|bsec|environment' } |
    Select-Object -ExpandProperty FullName
Write-Output "---DONE---"