Write-Host "ChillSense Windows setup helper" -ForegroundColor Cyan
Write-Host "==============================="
Write-Host ""

$code = Get-Command code -ErrorAction SilentlyContinue
if ($code) {
    Write-Host "Installing/reconfirming the Espressif ESP-IDF VS Code extension..."
    code --install-extension espressif.esp-idf-extension
} else {
    Write-Host "VS Code CLI ('code') was not found. Install VS Code first." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "ESP-IDF itself is intentionally installed through Espressif Installation Manager." -ForegroundColor Yellow
Write-Host "Install/select ESP-IDF v6.0.2 and the USB drivers when prompted."
Write-Host ""
Write-Host "After ESP-IDF is installed:"
Write-Host "  1. Open this repository in VS Code."
Write-Host "  2. Select target esp32s3."
Write-Host "  3. Connect through the UART USB-C port and select the CP210x COM port."
Write-Host "  4. Open SDK Configuration Editor and enter local Wi-Fi credentials."
Write-Host "  5. Build, Flash (UART), and Monitor."
Write-Host ""
Write-Host "Run .\scripts\check_environment.ps1 to verify the local environment."
