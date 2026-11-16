# Builds the RP2350 firmware images into build\kvm-target.uf2 (the keyboard
# side), build\kvm-bridge.uf2 (the plain bridge, only needed when a second
# RP2350 stands in for the ESP32 panel) and build\kvm-twoway.uf2 (both in one,
# the same image on both boards). Needs the Arduino "Raspberry Pi
# Pico/RP2040/RP2350" core (earlephilhower) and arduino-cli, which Arduino
# IDE 2 ships with.
#
#   .\build.ps1                 # Waveshare RP2350-Zero
#   .\build.ps1 -Board rpipico2

param([string]$Board = 'waveshare_rp2350_zero')

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli) {
    $cli = Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
}
if (-not (Test-Path $cli)) { throw 'arduino-cli not found. Install Arduino IDE 2 or put arduino-cli on PATH.' }

$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$images = @(
    @{ Name = 'kvm-bridge'; Sketch = 'kvm_bridge'; Fqbn = "rp2040:rp2040:$Board" },
    @{ Name = 'kvm-target'; Sketch = 'kvm_target'; Fqbn = "rp2040:rp2040:${Board}:usbstack=tinyusb" },
    @{ Name = 'kvm-twoway'; Sketch = 'kvm_twoway'; Fqbn = "rp2040:rp2040:${Board}:usbstack=tinyusb" }
)

foreach ($img in $images) {
    $work = Join-Path $env:TEMP "kvm-link-build\$Board\$($img.Sketch)"
    Write-Host "Building $($img.Name) ($($img.Fqbn))"
    & $cli compile --fqbn $img.Fqbn --build-path $work (Join-Path $root "firmware\$($img.Sketch)") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "$($img.Name) failed to build" }
    Copy-Item (Join-Path $work "$($img.Sketch).ino.uf2") (Join-Path $out "$($img.Name).uf2") -Force
}

Get-ChildItem $out -Filter *.uf2 | Select-Object Name, Length, LastWriteTime
