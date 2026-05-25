@echo off
setlocal
cd /d "%~dp0"

set "SCRIPT=%~dp0wechat_duplicate_monitor.py"
set "PID_FILE=%~dp0duplicate_monitor.pid"

if not exist "%PID_FILE%" (
    echo Duplicate monitor is not running, or PID file was not found.
    pause
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$pidFile = '%PID_FILE%'; $script = '%SCRIPT%'; $idText = (Get-Content -Raw -Path $pidFile).Trim(); if ($idText -notmatch '^\d+$') { Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue; Write-Host 'Invalid PID file removed.'; exit 0 }; $proc = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $idText) -ErrorAction SilentlyContinue; if (-not $proc) { Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue; Write-Host 'Duplicate monitor is not running. PID file removed.'; exit 0 }; if ($proc.CommandLine -notlike ('*' + $script + '*')) { Write-Host ('PID ' + $idText + ' is not this duplicate monitor. Not stopping it.'); exit 2 }; Stop-Process -Id ([int]$idText) -ErrorAction Stop; Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue; Write-Host ('Stopped duplicate monitor. PID: ' + $idText)"
if errorlevel 2 (
    pause
    exit /b 2
)
if errorlevel 1 (
    echo Failed to stop duplicate monitor.
    pause
    exit /b 1
)

pause
