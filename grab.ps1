# Saves one still from an HDMI capture card (or any camera) as a JPEG, using the camera API
# built into Windows, so nothing has to be installed.
#
#   .\grab.ps1                                    # first device whose name matches "capture"
#   .\grab.ps1 -Device Elgato -Out C:\temp\target.jpg
#   .\grab.ps1 -Width 1920                        # scaled so a pixel is a kvm.py coordinate
#   .\grab.ps1 -Width 1920 -Crop 0,0,640,360      # that part of the screen, in full detail
#   .\grab.ps1 -List
#
# -Width sets the size of the picture and so the coordinates everything else is measured in:
# with -Width 1920 a 3840x2160 capture is saved as 1920x1080, and a point in it is the point
# to give "kvm.py click" at its default --res 1920x1080.
#
# -Crop x,y,w,h keeps only that rectangle, given in those same coordinates, for a closer look
# at one part of the screen. It is saved with all the detail the capture has (never wider
# than -Width). The line printed at the end says how to turn a point in the cropped picture
# back into a screen coordinate.

param(
    [string]$Device = 'capture',
    [string]$Out = (Join-Path $env:TEMP 'kvm-frame.jpg'),
    [int]$WarmupMs = 1500,
    [int]$Width = 0,
    [string]$Crop,
    [switch]$List
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$asTaskOp = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
$asTaskAction = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
} | Select-Object -First 1

function Await($op, [Type]$type) {
    $task = $asTaskOp.MakeGenericMethod($type).Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    $task.Result
}
function AwaitAction($action) {
    $task = $asTaskAction.Invoke($null, @($action))
    $task.Wait(-1) | Out-Null
}

[void][Windows.Media.Capture.MediaCapture, Windows.Media.Capture, ContentType = WindowsRuntime]
[void][Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
[void][Windows.Storage.StorageFolder, Windows.Storage, ContentType = WindowsRuntime]
[void][Windows.Media.MediaProperties.ImageEncodingProperties, Windows.Media.MediaProperties, ContentType = WindowsRuntime]

$all = Await ([Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync([Windows.Devices.Enumeration.DeviceClass]::VideoCapture)) ([Windows.Devices.Enumeration.DeviceInformationCollection])
if ($List) { $all | ForEach-Object { $_.Name }; return }

$dev = $all | Where-Object { $_.Name -match $Device } | Select-Object -First 1
if (-not $dev) { throw "No video device matching '$Device'. Found: $(($all | ForEach-Object { $_.Name }) -join ', ')" }

$settings = New-Object Windows.Media.Capture.MediaCaptureInitializationSettings
$settings.VideoDeviceId = $dev.Id
$settings.StreamingCaptureMode = [Windows.Media.Capture.StreamingCaptureMode]::Video

$capture = New-Object Windows.Media.Capture.MediaCapture
try {
    AwaitAction ($capture.InitializeAsync($settings))
    $dir = Split-Path -Parent ([IO.Path]::GetFullPath($Out))
    $folder = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($dir)) ([Windows.Storage.StorageFolder])
    $jpeg = [Windows.Media.MediaProperties.ImageEncodingProperties]::CreateJpeg()
    $name = Split-Path -Leaf $Out

    # The first frame after opening a capture card is often black or stale: take one to
    # start the stream, wait, then take the one that is kept.
    $file = Await ($folder.CreateFileAsync($name, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
    AwaitAction ($capture.CapturePhotoToStorageFileAsync($jpeg, $file))
    Start-Sleep -Milliseconds $WarmupMs
    $file = Await ($folder.CreateFileAsync($name, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
    AwaitAction ($capture.CapturePhotoToStorageFileAsync($jpeg, $file))
} finally {
    $capture.Dispose()
}
if ($Crop) {
    $box = @($Crop -split '[ ,x]+' | Where-Object { $_ -ne '' } | ForEach-Object { [int]$_ })
    if ($box.Count -ne 4) { throw '-Crop takes four numbers: x,y,w,h' }
}
$note = ''
if ($Width -gt 0 -or $Crop) {
    Add-Type -AssemblyName System.Drawing
    $stream = New-Object IO.MemoryStream (, [IO.File]::ReadAllBytes($Out))
    $src = [Drawing.Image]::FromStream($stream)
    try {
        # The frame is the picture at -Width; f is capture pixels per frame pixel.
        $frameW = if ($Width -gt 0) { $Width } else { $src.Width }
        $f = $src.Width / $frameW
        $frameH = [int][Math]::Round($src.Height / $f)
        if ($Crop) {
            $x = [Math]::Max(0, [Math]::Min($box[0], $frameW - 1)); $y = [Math]::Max(0, [Math]::Min($box[1], $frameH - 1))
            $w = [Math]::Max(1, [Math]::Min($box[2], $frameW - $x)); $h = [Math]::Max(1, [Math]::Min($box[3], $frameH - $y))
        } else { $x = 0; $y = 0; $w = $frameW; $h = $frameH }
        # Keep the capture's own pixels unless that would be wider than the frame.
        $zoom = [Math]::Min($f, $frameW / $w)
        $outW = [int][Math]::Round($w * $zoom); $outH = [int][Math]::Round($h * $zoom)
        $bmp = New-Object Drawing.Bitmap $outW, $outH
        $g = [Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $from = New-Object Drawing.RectangleF ([single]($x * $f)), ([single]($y * $f)), ([single]($w * $f)), ([single]($h * $f))
        $to = New-Object Drawing.RectangleF 0, 0, $outW, $outH
        $g.DrawImage($src, $to, $from, [Drawing.GraphicsUnit]::Pixel)
        $g.Dispose()
        $codec = [Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
        $quality = New-Object Drawing.Imaging.EncoderParameters 1
        $quality.Param[0] = New-Object Drawing.Imaging.EncoderParameter ([Drawing.Imaging.Encoder]::Quality), 90L
        $bmp.Save($Out, $codec, $quality)
        $bmp.Dispose()
        $note = ", ${outW}x${outH} picture of a ${frameW}x${frameH} screen"
        if ($Crop) {
            $z = [Math]::Round($zoom, 3)
            $note += "; crop $x,$y,$w,$h at ${z}x: picture point (px,py) is screen point ($x + px/$z, $y + py/$z)"
        }
    } finally {
        $src.Dispose(); $stream.Dispose()
    }
}
"$($dev.Name) -> $Out ($((Get-Item $Out).Length) bytes$note)"
