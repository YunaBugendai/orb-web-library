# Orb! Windows bootstrap installer.
# Usage: irm https://raw.githubusercontent.com/YunaBugendai/orb-web-library/main/install.ps1 | iex
$ErrorActionPreference = 'Stop'
$repo = 'YunaBugendai/orb-web-library'
$api = "https://api.github.com/repos/$repo/releases/latest"
$release = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'Orb-Setup/1' }
$version = $release.tag_name.TrimStart('v')
$installerName = "Orb-Setup-$version.exe"
$asset = $release.assets | Where-Object { $_.name -eq $installerName } | Select-Object -First 1
if (-not $asset) {
    throw "The latest Orb! release does not contain $installerName."
}
$temp = Join-Path ([IO.Path]::GetTempPath()) ("Orb-Setup-" + [guid]::NewGuid().ToString() + '.exe')
try {
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $temp
    Write-Host "Starting Orb! Setup $($release.tag_name). Choose the components in the wizard."
    Start-Process -FilePath $temp -Wait
}
finally {
    if (Test-Path $temp) { Remove-Item $temp -Force }
}
