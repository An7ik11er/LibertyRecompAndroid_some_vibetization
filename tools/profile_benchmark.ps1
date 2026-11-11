<#
.SYNOPSIS
    Automated benchmark, teleport lock, and simpleperf profiling pipeline for LibertyRecomp Android.
    Device target: OnePlus 9RT (Snapdragon 888) or any connected ADB device.
.EXAMPLE
    .\traces\profile_benchmark.ps1
    .\traces\profile_benchmark.ps1 -DeviceId "2dc3ec42" -WaitSeconds 80 -ProfileDuration 10
#>
param(
    [string]$DeviceId = "2dc3ec42",
    [int]$WaitSeconds = 80,
    [int]$ProfileDuration = 10,
    [string]$SpotName = "test_spot"
)

$ErrorActionPreference = "Stop"

# 1. Resolve ADB toolchain
$adb = "G:\cod\cacheSlopodroid\platform-tools\adb.exe"
if (-not (Test-Path $adb)) {
    $adbCmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($adbCmd) {
        $adb = $adbCmd.Source
    } else {
        Write-Error "ADB not found at $adb or in PATH!"
    }
}
Write-Host "[1/7] Using ADB: $adb (Device: $DeviceId)" -ForegroundColor Cyan

# 2. Wake up and unlock device
Write-Host "[2/7] Waking up and unlocking device..." -ForegroundColor Cyan
& $adb -s $DeviceId shell "input keyevent KEYCODE_WAKEUP; input keyevent 82; wm dismiss-keyguard; input swipe 500 1800 500 300 150"
Start-Sleep -Seconds 1

# 3. Clean stale command/done flags
& $adb -s $DeviceId shell "rm -f /sdcard/Android/data/com.libertyrecomp/files/teleport.done /sdcard/Android/data/com.libertyrecomp/files/teleport.cmd"

# 4. Launch game with keep-screen-on & direct play
Write-Host "[3/7] Launching game (waiting $WaitSeconds seconds for gameplay load)..." -ForegroundColor Cyan
& $adb -s $DeviceId shell am start -n com.libertyrecomp/.LauncherActivity --ez play true
Start-Sleep -Seconds $WaitSeconds

# 5. Trigger teleport and locked benchmark camera
Write-Host "[4/7] Sending teleport trigger '$SpotName'..." -ForegroundColor Cyan
& $adb -s $DeviceId shell "cmd statusbar collapse; echo '$SpotName' > /sdcard/Android/data/com.libertyrecomp/files/teleport.cmd"
Start-Sleep -Seconds 5

$done = (& $adb -s $DeviceId shell "cat /sdcard/Android/data/com.libertyrecomp/files/teleport.done 2>/dev/null").Trim()
Write-Host "      Teleport status: $done" -ForegroundColor Green

# 6. Resolve game engine PID and capture screenshot
$gpid = (& $adb -s $DeviceId shell "pidof com.libertyrecomp:game 2>/dev/null").Trim()
if (-not $gpid) {
    Write-Warning "Could not find 'com.libertyrecomp:game' process! Attempting general pidof..."
    $gpid = (& $adb -s $DeviceId shell "pidof com.libertyrecomp 2>/dev/null").Trim()
}

if (-not $gpid) {
    Write-Error "Game process is not running! Check logcat."
}

Write-Host "[5/7] Game PID: $gpid. Capturing benchmark screenshot..." -ForegroundColor Cyan
$screenOut = Join-Path $PSScriptRoot "latest_benchmark_screen.png"
& $adb -s $DeviceId shell "screencap -p /sdcard/Android/data/com.libertyrecomp/files/benchmark_screen.png"
& $adb -s $DeviceId pull /sdcard/Android/data/com.libertyrecomp/files/benchmark_screen.png "$screenOut" | Out-Null
Write-Host "      Saved screenshot to $screenOut" -ForegroundColor Green

# 7. Record simpleperf profile
Write-Host "[6/7] Recording $ProfileDuration s simpleperf profile on PID $gpid..." -ForegroundColor Cyan
& $adb -s $DeviceId shell "run-as com.libertyrecomp rm -f /data/data/com.libertyrecomp/perf_latest.data"
& $adb -s $DeviceId shell "run-as com.libertyrecomp simpleperf record -p $gpid -e cpu-cycles -f 4000 --duration $ProfileDuration -o /data/data/com.libertyrecomp/perf_latest.data"

# 8. Force-stop game immediately to preserve thermal headroom
Write-Host "[7/7] Stopping game process to cool SoC..." -ForegroundColor Cyan
& $adb -s $DeviceId shell am force-stop com.libertyrecomp
Write-Host "      Game stopped cleanly." -ForegroundColor Green

# 9. Output report
Write-Host "`n================================ THREAD BREAKDOWN ================================" -ForegroundColor Yellow
& $adb -s $DeviceId shell "run-as com.libertyrecomp simpleperf report -i /data/data/com.libertyrecomp/perf_latest.data --sort comm -n"

Write-Host "`n============================ TOP RENDERGUEST SYMBOLS =============================" -ForegroundColor Yellow
& $adb -s $DeviceId shell "run-as com.libertyrecomp simpleperf report -i /data/data/com.libertyrecomp/perf_latest.data --sort dso,symbol -n --comms RenderGuest | head -n 35"

Write-Host "`n============================ TOP MAIN XTHREAD SYMBOLS ============================" -ForegroundColor Yellow
& $adb -s $DeviceId shell "run-as com.libertyrecomp simpleperf report -i /data/data/com.libertyrecomp/perf_latest.data --sort dso,symbol -n --comms 'Main XThread (F' | head -n 25"
