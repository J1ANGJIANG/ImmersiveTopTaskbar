Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'icon.png'
$out = Join-Path $root 'app.ico'
$sizes = @(16, 32, 48, 256)

$srcImg = [System.Drawing.Image]::FromFile($src)

# 每个尺寸生成 PNG 字节
$entries = @()
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.SmoothingMode = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'
    $g.DrawImage($srcImg, 0, 0, $s, $s)
    $g.Dispose()

    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $png = $ms.ToArray()
    $ms.Dispose()
    $bmp.Dispose()

    $w = if ($s -ge 256) { [byte]0 } else { [byte]$s }
    $entries += [PSCustomObject]@{
        w = $w; h = $w; colors = [byte]0; reserved = [byte]0
        planes = [uint16]1; bpp = [uint16]32
        size = [uint32]$png.Length; data = $png
    }
}
$srcImg.Dispose()

# 组装 ICO
$count = $entries.Count
$headerSize = 6 + 16 * $count
$offset = $headerSize
$total = $headerSize
foreach ($e in $entries) { $total += $e.data.Length }

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$bw.Write([uint16]0)          # reserved
$bw.Write([uint16]1)          # type = 1 (icon)
$bw.Write([uint16]$count)

$offset = $headerSize
foreach ($e in $entries) {
    $bw.Write([byte]$e.w)
    $bw.Write([byte]$e.h)
    $bw.Write([byte]$e.colors)
    $bw.Write([byte]$e.reserved)
    $bw.Write([uint16]$e.planes)
    $bw.Write([uint16]$e.bpp)
    $bw.Write([uint32]$e.size)
    $bw.Write([uint32]$offset)
    $offset += $e.size
}
foreach ($e in $entries) {
    $bw.Write($e.data)   # byte[] overload
}
$bw.Flush()
[System.IO.File]::WriteAllBytes($out, $ms.ToArray())
$bw.Dispose(); $ms.Dispose()

Write-Output ("Generated {0} : {1} bytes, sizes {2}" -f $out, (Get-Item $out).Length, ($sizes -join ','))
