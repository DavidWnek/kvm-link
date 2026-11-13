# Builds the ESP32 bridge firmware and writes it to the panel.
#
#   .\flash-bridge.ps1                      # JC3248W535C, port found automatically
#   .\flash-bridge.ps1 -Env crow70 -Port COM7
#
# PlatformIO's own upload step ships an older esptool that cannot get an ESP32-S3 with
# native USB into or out of flashing mode on this PC, so this uses the newer esptool that
# comes with the Arduino ESP32 package when it is there.

param([string]$Env = 'jc3248w535', [string]$Port)

$ErrorActionPreference = 'Stop'
$proj = Join-Path $PSScriptRoot 'firmware\kvm_bridge_esp32'

if (-not $Port) {
    $Port = (python -c "from serial.tools import list_ports; print(next((p.device for p in list_ports.comports() if p.vid in (0x303A, 0x1A86, 0x10C4)), ''))").Trim()
    if (-not $Port) { throw 'No ESP32 serial port found. Pass -Port COMx.' }
}

Push-Location $proj
try {
    python -m platformio run -e $Env | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "build failed; run 'python -m platformio run -e $Env' in $proj to see why" }
} finally { Pop-Location }

$build = Join-Path $proj ".pio\build\$Env"
$esptool = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py" -Recurse -Filter esptool.exe -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $esptool) {
    throw "Newer esptool not found. Fall back to: python -m platformio run -e $Env -t upload --upload-port $Port (hold BOOT and tap RST first, tap RST afterwards)."
}

$boot_app0 = Join-Path $env:USERPROFILE '.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin'
Write-Host "Writing $Env to $Port"
$ErrorActionPreference = 'Continue' # esptool reports progress on stderr
$log = & $esptool --chip esp32s3 --port $Port --before usb-reset --after watchdog-reset write-flash `
    0x0 (Join-Path $build 'bootloader.bin') `
    0x8000 (Join-Path $build 'partitions.bin') `
    0xe000 $boot_app0 `
    0x10000 (Join-Path $build 'firmware.bin') 2>&1 | ForEach-Object { "$_" }
# The watchdog reset drops the USB port under esptool, which then exits non-zero even though
# everything was written, so success is four verified images rather than the exit code.
$verified = @($log | Where-Object { $_ -match 'Hash of data verified' }).Count
if ($verified -lt 4) {
    $log | Where-Object { $_ -and $_ -notmatch 'Writing at' } | Select-Object -Last 12 | Write-Host
    throw 'upload failed. Hold BOOT, tap RST, release BOOT, and run this again.'
}
Write-Host 'Written and verified; the panel is restarting.'
