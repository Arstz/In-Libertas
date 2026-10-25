[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SourceDirectory,
    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Write-Icon {
    param([string] $OutputPath, [string] $SmallImagePath, [string] $LargeImagePath)

    $extents = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    $smallImage = [System.Drawing.Image]::FromFile($SmallImagePath)
    $largeImage = [System.Drawing.Image]::FromFile($LargeImagePath)
    $frames = [System.Collections.Generic.List[byte[]]]::new()
    try {
        foreach ($extent in $extents) {
            $image = if ($extent -le 48) { $smallImage } else { $largeImage }
            $bitmap = [System.Drawing.Bitmap]::new($extent, $extent)
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            $stream = [System.IO.MemoryStream]::new()
            try {
                $graphics.Clear([System.Drawing.Color]::Transparent)
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $scale = [Math]::Min($extent / $image.Width, $extent / $image.Height)
                $width = [int][Math]::Round($image.Width * $scale)
                $height = [int][Math]::Round($image.Height * $scale)
                $graphics.DrawImage($image, [int](($extent - $width) / 2), [int](($extent - $height) / 2), $width, $height)
                $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
                $frames.Add($stream.ToArray())
            } finally {
                $stream.Dispose()
                $graphics.Dispose()
                $bitmap.Dispose()
            }
        }

        $writer = [System.IO.BinaryWriter]::new([System.IO.File]::Create($OutputPath))
        try {
            $writer.Write([uint16]0)
            $writer.Write([uint16]1)
            $writer.Write([uint16]$extents.Count)
            $offset = 6 + 16 * $extents.Count
            for ($index = 0; $index -lt $extents.Count; ++$index) {
                $dimension = if ($extents[$index] -eq 256) { 0 } else { $extents[$index] }
                $writer.Write([byte]$dimension)
                $writer.Write([byte]$dimension)
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([uint16]1)
                $writer.Write([uint16]32)
                $writer.Write([uint32]$frames[$index].Length)
                $writer.Write([uint32]$offset)
                $offset += $frames[$index].Length
            }
            foreach ($frame in $frames) {
                $writer.Write($frame)
            }
        } finally {
            $writer.Dispose()
        }
    } finally {
        $smallImage.Dispose()
        $largeImage.Dispose()
    }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$logo = Join-Path $SourceDirectory 'Logo.png'
Write-Icon -OutputPath (Join-Path $OutputDirectory 'application.ico') -SmallImagePath $logo -LargeImagePath $logo
Write-Icon -OutputPath (Join-Path $OutputDirectory 'project.ico') -SmallImagePath (Join-Path $SourceDirectory 'File small.png') -LargeImagePath (Join-Path $SourceDirectory 'File big.png')
