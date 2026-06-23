# Generates the "My Media Vault" app icon: a play button inside a vault ring on a blue->purple gradient.
# Produces appicon.png (256) and a multi-size appicon.ico.
Add-Type -AssemblyName System.Drawing

$c1 = [System.Drawing.Color]::FromArgb(255, 0x46, 0x6E, 0xD8)  # blue
$c2 = [System.Drawing.Color]::FromArgb(255, 0x8A, 0x4F, 0xC8)  # purple
$white = [System.Drawing.Color]::FromArgb(245, 255, 255, 255)

function RoundRectPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Draw([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [single]([Math]::Max(1, [int]($S * 0.03)))
    $r = [single]($S * 0.20)
    $path = RoundRectPath $m $m ([single]($S - 2 * $m)) ([single]($S - 2 * $m)) $r
    $rect = New-Object System.Drawing.Rectangle(0, 0, $S, $S)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 45.0)
    $g.FillPath($grad, $path)

    $cx = [single]($S / 2.0); $cy = [single]($S / 2.0)
    $ringR = [single]($S * 0.31)
    $penW = [single]([Math]::Max(2.0, $S * 0.055))
    $pen = New-Object System.Drawing.Pen($white, $penW)
    $g.DrawEllipse($pen, ($cx - $ringR), ($cy - $ringR), ($ringR * 2), ($ringR * 2))

    # vault "bolt" ticks around the ring
    $tickBrush = New-Object System.Drawing.SolidBrush($white)
    for ($i = 0; $i -lt 8; $i++) {
        $a = [Math]::PI * 2 * $i / 8
        $tr = $ringR + $penW * 0.9
        $td = [single]([Math]::Max(2.0, $S * 0.022))
        $tx = $cx + [single]([Math]::Cos($a)) * $tr - $td / 2
        $ty = $cy + [single]([Math]::Sin($a)) * $tr - $td / 2
        $g.FillEllipse($tickBrush, $tx, $ty, $td, $td)
    }

    # play triangle in the centre
    $pts = @(
        (New-Object System.Drawing.PointF(($cx - $S * 0.095), ($cy - $S * 0.145))),
        (New-Object System.Drawing.PointF(($cx - $S * 0.095), ($cy + $S * 0.145))),
        (New-Object System.Drawing.PointF(($cx + $S * 0.165), $cy))
    )
    $g.FillPolygon($tickBrush, [System.Drawing.PointF[]]$pts)

    $g.Dispose()
    return $bmp
}

function PngBytes($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bytes = $ms.ToArray()
    $ms.Dispose()
    return , $bytes
}

$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Save the 256 PNG (used for the in-app window icon).
$big = Draw 256
$big.Save((Join-Path $dir 'appicon.png'), [System.Drawing.Imaging.ImageFormat]::Png)

# Build a multi-size ICO (PNG-compressed entries; fine on modern Windows).
$sizes = @(256, 64, 48, 32, 16)
$pngs = @()
foreach ($s in $sizes) { $pngs += , (PngBytes (Draw $s)) }

$icoPath = Join-Path $dir 'appicon.ico'
$fs = New-Object System.IO.FileStream($icoPath, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)            # reserved
$bw.Write([UInt16]1)            # type = icon
$bw.Write([UInt16]$sizes.Count) # image count
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $pngs[$i].Length
    $bw.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))  # width  (0 => 256)
    $bw.Write([Byte]($(if ($s -ge 256) { 0 } else { $s })))  # height (0 => 256)
    $bw.Write([Byte]0)             # palette
    $bw.Write([Byte]0)             # reserved
    $bw.Write([UInt16]1)           # planes
    $bw.Write([UInt16]32)          # bpp
    $bw.Write([UInt32]$len)        # bytes in resource
    $bw.Write([UInt32]$offset)     # offset
    $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush(); $bw.Close(); $fs.Close()

Write-Output ("wrote appicon.png and appicon.ico ({0} sizes) to {1}" -f $sizes.Count, $dir)
