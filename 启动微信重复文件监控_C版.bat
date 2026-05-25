@echo off
setlocal
cd /d "%~dp0"

set "EXE=%~dp0wechat_duplicate_monitor.exe"
set "ROOT_DIR=%~dp0.."
set "PID_FILE=%~dp0duplicate_monitor_c.pid"
set "LOG_FILE=%~dp0duplicate_monitor_c.log"

if not exist "%EXE%" (
    echo Cannot find wechat_duplicate_monitor.exe in this folder.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$pidFile = '%PID_FILE%'; $exe = (Resolve-Path '%EXE%').Path; if (Test-Path $pidFile) { $idText = (Get-Content -Raw -Path $pidFile).Trim(); if ($idText -match '^\d+$') { $proc = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $idText) -ErrorAction SilentlyContinue; if ($proc -and (($proc.ExecutablePath -eq $exe) -or ($proc.CommandLine -like ('*' + $exe + '*')))) { Write-Host ('Already running. PID: ' + $idText); exit 10 } } Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue }; exit 0"
if errorlevel 10 (
    pause
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$workDir = '%~dp0'; $exe = (Resolve-Path '%EXE%').Path; $rootDir = (Resolve-Path '%ROOT_DIR%').Path; $pidFile = '%PID_FILE%'; $logFile = '%LOG_FILE%'; $argList = @('--root', $rootDir, '--action', 'move', '--log-file', $logFile); $p = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $workDir -WindowStyle Hidden -PassThru; Start-Sleep -Seconds 2; $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue; if (-not $proc) { Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue; Write-Host 'Duplicate monitor C version started but exited immediately.'; if (Test-Path $logFile) { Write-Host ('Monitor log: ' + $logFile); Get-Content -Path $logFile -Tail 20 }; exit 2 }; Set-Content -Path $pidFile -Value $p.Id -Encoding ASCII; Write-Host ('Started duplicate monitor C version in move mode. PID: ' + $p.Id); Write-Host ('Executable: ' + $exe); Write-Host ('Root folder: ' + $rootDir); Write-Host ('Log file: ' + $logFile)"
if errorlevel 2 (
    pause
    exit /b 2
)
if errorlevel 1 (
    echo Failed to start duplicate monitor C version.
    pause
    exit /b 1
)

exit /b 0
