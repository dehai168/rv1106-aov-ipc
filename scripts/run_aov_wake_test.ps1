# Host-side AOV wake loop (Windows adb). Usage: .\scripts\run_aov_wake_test.ps1 [-Count 10] [-Ms 1000]
param(
    [int]$Count = 10,
    [int]$Ms = 1000
)

$ErrorActionPreference = "Continue"
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) { $adb = "C:\Users\dehai\Documents\Sdks\adb-platform-tools\adb.exe" }

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$logDir = Join-Path $root "build\aov-wake-test"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$hostLog = Join-Path $logDir ("run_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))

function Log($msg) {
    $line = "[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg
    Write-Host $line
    Add-Content -Path $hostLog -Value $line
}

& $adb wait-for-device
Log "push on-board helper"
& $adb push (Join-Path $root "scripts\test_aov_wake.sh") /mnt/sdcard/test_aov_wake.sh
& $adb shell "chmod +x /mnt/sdcard/test_aov_wake.sh"

$ok = 0
$fail = 0

for ($i = 1; $i -le $Count; $i++) {
    Log "=== cycle $i/$Count ==="
    $upt = (& $adb shell "awk '{print int(`$1)}' /proc/uptime").Trim()
    Log "before uptime=$upt"

    & $adb shell "nohup sh /mnt/sdcard/test_aov_wake.sh 1 $Ms > /mnt/sdcard/aov_wake_once.log 2>&1 & echo started"
    Start-Sleep -Seconds 2

    $gone = $false
    for ($t = 0; $t -lt 20; $t++) {
        $devs = & $adb devices
        if ($devs -notmatch '\s+device\s*$') { $gone = $true; break }
        Start-Sleep -Seconds 1
    }

    if (-not $gone) {
        Log "WARN: adb still up after 20s — suspend may not have happened"
        $tail = (& $adb shell "tail -5 /mnt/sdcard/aov_wake_once.log").Trim()
        Log $tail
        $fail++
        continue
    }

    Log "adb disconnected (suspend likely)"
    & $adb wait-for-device
    Start-Sleep -Seconds 3

    $upt2 = (& $adb shell "awk '{print int(`$1)}' /proc/uptime").Trim()
    $mark = (& $adb shell "cat /mnt/sdcard/aov_wake_mark.txt 2>/dev/null").Trim()
    $video = (& $adb shell "test -c /dev/video0 && echo 1 || echo 0").Trim()
    $state = (& $adb shell "cat /sys/power/state").Trim()
    $delta = [int]$upt2 - [int]$upt
    Log "after uptime=$upt2 delta=$delta mark=$mark video=$video state=$state"

    if ([int]$upt2 -gt [int]$upt -and $delta -lt 600 -and $video -eq "1") {
        $ok++
        Log "cycle $i PASS"
    } else {
        $fail++
        Log "cycle $i FAIL"
    }
    Start-Sleep -Seconds 2
}

Log "=== summary ok=$ok fail=$fail total=$Count ==="
& $adb shell "tail -20 /mnt/sdcard/aov_wake_test.log 2>/dev/null"
