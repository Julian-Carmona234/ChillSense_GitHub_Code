Write-Host "ChillSense ESP32 environment check" -ForegroundColor Cyan
Write-Host "=================================="

function Show-Check($name, $ok, $detail) {
    if ($ok) {
        Write-Host "[OK]      $name - $detail" -ForegroundColor Green
    } else {
        Write-Host "[MISSING] $name - $detail" -ForegroundColor Yellow
    }
}

$git = Get-Command git -ErrorAction SilentlyContinue
Show-Check "Git" ($null -ne $git) ($(if ($git) { (git --version) } else { "Install Git" }))

$code = Get-Command code -ErrorAction SilentlyContinue
Show-Check "VS Code CLI" ($null -ne $code) ($(if ($code) { "available" } else { "VS Code may still be installed; enable the 'code' command if desired" }))

if ($code) {
    $ext = code --list-extensions 2>$null | Select-String -SimpleMatch "espressif.esp-idf-extension"
    Show-Check "ESP-IDF VS Code extension" ($null -ne $ext) ($(if ($ext) { "installed" } else { "run: code --install-extension espressif.esp-idf-extension" }))
}

$idfCandidates = @(
    $env:IDF_PATH,
    "C:\esp\v6.0.2\esp-idf"
) | Where-Object { $_ -and (Test-Path $_) }

if ($idfCandidates.Count -gt 0) {
    $idfPath = $idfCandidates[0]
    Show-Check "ESP-IDF" $true $idfPath
} else {
    Show-Check "ESP-IDF" $false "Expected v6.0.2; install with Espressif Installation Manager"
}

try {
    $ports = Get-PnpDevice -PresentOnly -ErrorAction Stop |
        Where-Object { $_.FriendlyName -match "CP210|USB to UART" }

    if ($ports) {
        $names = ($ports | ForEach-Object { $_.FriendlyName }) -join "; "
        Show-Check "CP210x UART interface" $true $names
    } else {
        Show-Check "CP210x UART interface" $false "Connect the ESP32 through its UART USB-C port or install the CP210x driver"
    }
} catch {
    Write-Host "[INFO]    Could not query Plug-and-Play devices in this PowerShell session."
}

Write-Host ""
Write-Host "Required project target: esp32s3"
Write-Host "Required ESP-IDF version: v6.0.2"
Write-Host "Remember: configure Wi-Fi locally in menuconfig; sdkconfig is not committed."
