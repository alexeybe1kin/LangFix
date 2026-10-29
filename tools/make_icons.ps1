# Draws the tray/app icons (rounded square with two swap arrows) into res\on.ico and res\off.ico.
# Each .ico holds PNG frames at the sizes Windows asks for at 100-300% scaling.
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$res = Join-Path $root 'res'
New-Item -ItemType Directory -Force $res | Out-Null

function Draw-Frame([int]$s, [System.Drawing.Color]$c1, [System.Drawing.Color]$c2) {
  $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.PixelOffsetMode = 'HighQuality'
  $r = [Math]::Max(2.0, $s * 0.22)
  $m = [Math]::Max(0.5, $s * 0.03)
  $w = $s - 2 * $m
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $path.AddArc($m, $m, 2 * $r, 2 * $r, 180, 90)
  $path.AddArc($m + $w - 2 * $r, $m, 2 * $r, 2 * $r, 270, 90)
  $path.AddArc($m + $w - 2 * $r, $m + $w - 2 * $r, 2 * $r, 2 * $r, 0, 90)
  $path.AddArc($m, $m + $w - 2 * $r, 2 * $r, 2 * $r, 90, 90)
  $path.CloseFigure()
  $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $s, $s), $c1, $c2
  $g.FillPath($brush, $path)

  # two arrows: top one points right, bottom one points left
  $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(1.6, $s * 0.1))
  $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
  $x0 = $s * 0.24; $x1 = $s * 0.76; $h = $s * 0.13
  $y1 = $s * 0.36; $y2 = $s * 0.64
  $g.DrawLine($pen, $x0, $y1, $x1, $y1)
  $g.DrawLines($pen, [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF ($x1 - $h), ($y1 - $h)), (New-Object System.Drawing.PointF $x1, $y1), (New-Object System.Drawing.PointF ($x1 - $h), ($y1 + $h))))
  $g.DrawLine($pen, $x1, $y2, $x0, $y2)
  $g.DrawLines($pen, [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF ($x0 + $h), ($y2 - $h)), (New-Object System.Drawing.PointF $x0, $y2), (New-Object System.Drawing.PointF ($x0 + $h), ($y2 + $h))))
  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return ,$ms.ToArray()
}

function Write-Ico([string]$file, [System.Drawing.Color]$c1, [System.Drawing.Color]$c2) {
  $sizes = 16, 20, 24, 32, 40, 48, 64, 256
  $frames = foreach ($s in $sizes) { ,(Draw-Frame $s $c1 $c2) }
  $out = New-Object System.IO.MemoryStream
  $bw = New-Object System.IO.BinaryWriter $out
  $bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
  $offset = 6 + 16 * $sizes.Count
  for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $len = $frames[$i].Length
    $bw.Write([byte]($s % 256)); $bw.Write([byte]($s % 256)); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
    $offset += $len
  }
  foreach ($f in $frames) { $bw.Write([byte[]]$f) }
  $bw.Flush()
  [System.IO.File]::WriteAllBytes($file, $out.ToArray())
}

Write-Ico (Join-Path $res 'on.ico')  ([System.Drawing.Color]::FromArgb(255, 59, 130, 246)) ([System.Drawing.Color]::FromArgb(255, 124, 58, 237))
Write-Ico (Join-Path $res 'off.ico') ([System.Drawing.Color]::FromArgb(255, 148, 163, 184)) ([System.Drawing.Color]::FromArgb(255, 100, 116, 139))
Get-ChildItem $res -Filter *.ico | ForEach-Object { "$($_.Name) $($_.Length) bytes" }
