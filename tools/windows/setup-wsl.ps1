# Installs Orb! inside WSL and puts an `orb` command on your Windows PATH.
# Run from PowerShell:   powershell -ExecutionPolicy Bypass -File tools\windows\setup-wsl.ps1
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Write-Host "WSL is not installed. Open PowerShell as Administrator, run:  wsl --install"
    Write-Host "then reboot, finish the Ubuntu first-run setup, and run this script again."
    exit 1
}

$wslRoot = (wsl.exe wslpath -a "$root").Trim()
Write-Host "Building and installing Orb! inside WSL (you may be asked for your Linux password)..."
wsl.exe -e sh -c "sudo apt-get update && sudo apt-get install -y build-essential binutils make && cd '$wslRoot' && make && sudo make install && orb doctor"
if ($LASTEXITCODE -ne 0) { Write-Host "Setup failed (see messages above)."; exit 1 }

$bin = Join-Path $env:USERPROFILE ".orb\bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null
Copy-Item "$PSScriptRoot\orb.cmd" (Join-Path $bin "orb.cmd") -Force
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($userPath -notlike "*$bin*") {
    [Environment]::SetEnvironmentVariable('Path', "$userPath;$bin", 'User')
    Write-Host "Added $bin to your PATH. Open a new terminal to use the `orb` command."
}
Write-Host "Done. Try:  orb version"
