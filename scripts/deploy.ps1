# Deploy ipc_app to board via adb and run in foreground (logs to console).
param(
  [string]$RemotePath = "/userdata/ipc_app",
  [string]$LocalBin = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $LocalBin) {
  $LocalBin = Join-Path $Root "build\ipc_app"
}

if (-not (Test-Path $LocalBin)) {
  Write-Error "Binary not found: $LocalBin  (run scripts/build.sh in WSL first)"
}

$devices = adb devices | Select-String "`tdevice$"
if (-not $devices) {
  Write-Error "No adb device. Replug USB / power-cycle board."
}

Write-Host "Push $LocalBin -> $RemotePath"
adb push $LocalBin $RemotePath
adb shell "chmod +x $RemotePath"

Write-Host "Run $RemotePath"
adb shell "$RemotePath"
