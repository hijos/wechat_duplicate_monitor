@echo off
setlocal
cd /d "%~dp0"

set "SCRIPT=%~dp0wechat_duplicate_monitor.py"
set "ROOT_DIR=%~dp0.."
set "PID_FILE=%~dp0duplicate_monitor.pid"
set "LOG_FILE=%~dp0duplicate_monitor.log"

if not exist "%SCRIPT%" (
    echo Cannot find wechat_duplicate_monitor.py in this folder.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$pidFile = '%PID_FILE%'; $script = '%SCRIPT%'; if (Test-Path $pidFile) { $idText = (Get-Content -Raw -Path $pidFile).Trim(); if ($idText -match '^\d+$') { $proc = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $idText) -ErrorAction SilentlyContinue; if ($proc -and $proc.CommandLine -like ('*' + $script + '*')) { Write-Host ('Already running. PID: ' + $idText); exit 10 } } Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue }; exit 0"
if errorlevel 10 (
    pause
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "$workDir = '%~dp0'; $script = '%SCRIPT%'; $rootDir = (Resolve-Path '%ROOT_DIR%').Path; $pidFile = '%PID_FILE%'; $logFile = '%LOG_FILE%'; $python = $null; $pythonCmd = Get-Command python -ErrorAction SilentlyContinue; if ($pythonCmd) { $pythonDir = Split-Path -Parent $pythonCmd.Source; $pythonw = Join-Path $pythonDir 'pythonw.exe'; if (Test-Path $pythonw) { $python = $pythonw } else { $python = $pythonCmd.Source } }; if (-not $python) { $pyCmd = Get-Command py -ErrorAction SilentlyContinue; if ($pyCmd) { $python = $pyCmd.Source } }; if (-not $python) { Write-Host 'Python was not found. Please install Python 3 or add it to PATH.'; exit 1 }; $argList = @($script, '--root', $rootDir, '--action', 'move', '--log-file', $logFile); $isPythonw = [IO.Path]::GetFileName($python).ToLowerInvariant() -eq 'pythonw.exe'; if (-not $isPythonw) { $argList = @('-u') + $argList }; $p = Start-Process -FilePath $python -ArgumentList $argList -WorkingDirectory $workDir -WindowStyle Hidden -PassThru; Start-Sleep -Seconds 2; $proc = Get-Process -Id $p.Id -ErrorAction SilentlyContinue; if (-not $proc) { Remove-Item -Path $pidFile -Force -ErrorAction SilentlyContinue; Write-Host 'Duplicate monitor started but exited immediately.'; if (Test-Path $logFile) { Write-Host ('Monitor log: ' + $logFile); Get-Content -Path $logFile -Tail 20 }; exit 2 }; Set-Content -Path $pidFile -Value $p.Id -Encoding ASCII; Write-Host ('Started duplicate monitor in move mode. PID: ' + $p.Id); Write-Host ('Python: ' + $python); Write-Host ('Root folder: ' + $rootDir); Write-Host ('Log file: ' + $logFile)"
if errorlevel 2 (
    pause
    exit /b 2
)
if errorlevel 1 (
    echo Failed to start duplicate monitor.
    pause
    exit /b 1
)

exit /b 0
