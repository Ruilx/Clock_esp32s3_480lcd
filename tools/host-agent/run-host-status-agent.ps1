param(
    [string]$EspIp = "10.4.212.47",
    [string]$Gpu = "auto",
    [int]$IntervalMs = 1000
)

$agent = Join-Path $PSScriptRoot "build\host-status-agent.exe"
if (-not (Test-Path $agent)) {
    throw "Missing host agent executable: $agent"
}

$process = Start-Process -FilePath $agent `
    -ArgumentList "--esp-ip",$EspIp,"--gpu",$Gpu,"--interval",$IntervalMs,"--headless" `
    -WindowStyle Hidden `
    -PassThru

Write-Host "Host status agent started. PID=$($process.Id)"
