# The kit's screenshot tool: saves a PNG of a program's window, for the README's pictures (docs/images/).
# Usage: powershell -ExecutionPolicy Bypass -File .claude/screenshot.ps1 -Out docs/images/menu.png (-Process name | -Title "part of the title") [-Wide169] [-Blur boxes]
#        powershell -ExecutionPolicy Bypass -File .claude/screenshot.ps1 -Out docs/images/menu.png -Blur "72,385,500,22"   (blur a saved picture)
#   -Process  the process name without .exe (e.g. stilwater); -Title matches part of the window title instead
#   -Wide169  keep only the middle 16:9 of a wider window (an ultrawide's side bars, where a game draws nothing)
#   -Blur     "x,y,width,height" boxes in the picture's pixels, separated by ";", pixelated beyond reading: a file
#             path, a name, chat. Without -Process or -Title it blurs the picture already at -Out.
# Look at every picture before committing it: no names, chat, file paths or other people, and default settings.
param([Parameter(Mandatory)][string]$Out, [string]$Process = "", [string]$Title = "", [switch]$Wide169, [string]$Blur = "")
$ErrorActionPreference = "Stop"  # a failed save must not still print "saved"
Add-Type -AssemblyName System.Drawing
$full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Out)  # relative or absolute

# Pixelates each box with blocks at least as tall as a line of text (up to 32 px), so text turns into plain cells.
function Pixelate($bmp, [string]$spec) {
  $count = 0
  foreach ($box in ($spec -split ";" | Where-Object { $_.Trim() })) {
    $n = @($box -split "," | ForEach-Object { [int]$_.Trim() })
    if ($n.Count -ne 4) { throw "-Blur takes x,y,width,height boxes separated by ';', not '$box'" }
    $x = [Math]::Max(0, $n[0]); $y = [Math]::Max(0, $n[1])
    $w = [Math]::Min($n[0] + $n[2], $bmp.Width) - $x; $h = [Math]::Min($n[1] + $n[3], $bmp.Height) - $y
    if ($w -le 0 -or $h -le 0) { throw "blur box '$box' is outside the $($bmp.Width)x$($bmp.Height) picture" }
    $block = [Math]::Max(12, [Math]::Min([Math]::Min($w, $h), 32))
    # Solid cells (no alpha): a see-through cell would blend with the text under it and leave it readable.
    $small = New-Object System.Drawing.Bitmap ([int][Math]::Ceiling($w / $block)), ([int][Math]::Ceiling($h / $block)),
      ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $edges = New-Object System.Drawing.Imaging.ImageAttributes
    $edges.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)  # no transparent samples past the box's edge
    $g = [System.Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear  # each cell: the average
    $g.DrawImage($bmp, (New-Object System.Drawing.Rectangle 0, 0, $small.Width, $small.Height), $x, $y, $w, $h,
      [System.Drawing.GraphicsUnit]::Pixel, $edges)
    $g.Dispose(); $edges.Dispose()
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy  # replace the box, never blend
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor  # back up as hard blocks
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($small, (New-Object System.Drawing.Rectangle $x, $y, $w, $h),
      (New-Object System.Drawing.Rectangle 0, 0, $small.Width, $small.Height), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose(); $small.Dispose(); $count++
  }
  $count
}

if (-not $Process -and -not $Title) {
  if (-not $Blur) { throw "give -Process or -Title to take a picture, or -Blur to blur the one at -Out" }
  if (-not (Test-Path -LiteralPath $full)) { throw "no picture at $Out to blur" }
  $file = [System.Drawing.Image]::FromFile($full)
  $bmp = New-Object System.Drawing.Bitmap $file; $file.Dispose()  # a copy, so the file can be written over
  $n = Pixelate $bmp $Blur
  $bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  "blurred $n box(es) in $Out. Look at it again."
  return
}

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class KitShot {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
}
"@ -ReferencedAssemblies System.Drawing
# Real pixels on every monitor, even when they're scaled differently (per-monitor aware; -4 = PER_MONITOR_AWARE_V2).
# Only system-aware, a window on a monitor scaled unlike the main one is looked for in the wrong place.
if ([KitShot]::SetThreadDpiAwarenessContext([IntPtr]::new(-4)) -eq [IntPtr]::Zero) { [KitShot]::SetProcessDPIAware() | Out-Null }

$windows = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 }
$p = if ($Process) { $windows | Where-Object { $_.ProcessName -eq $Process } | Select-Object -First 1 }
     else { $windows | Where-Object { $_.MainWindowTitle -like "*$Title*" } | Select-Object -First 1 }
if (-not $p) { throw "no window for $(if ($Process) { "process '$Process'" } else { "title '$Title'" })" }
$h = $p.MainWindowHandle
if ([KitShot]::IsIconic($h)) { [KitShot]::ShowWindow($h, 9) | Out-Null }  # 9 = restore a minimised window
[KitShot]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400  # let it come to the front and redraw

$r = New-Object KitShot+R; [KitShot]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object System.Drawing.Point 0, 0; [KitShot]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $r.Ri - $r.L; $hgt = $r.B - $r.T
if ($w -le 0 -or $hgt -le 0) { throw "the window has no visible area" }
$x = 0
if ($Wide169 -and $w * 9 -gt $hgt * 16) { $x = [int](($w - $hgt * 16 / 9) / 2); $w = [int]($hgt * 16 / 9) }
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
# Copied from the screen, not PrintWindow: games drawn with DirectX or Vulkan come out black through PrintWindow.
$g.CopyFromScreen((New-Object System.Drawing.Point ($pt.X + $x), $pt.Y), [System.Drawing.Point]::Empty, $bmp.Size)
$g.Dispose()
if ($Blur) { Pixelate $bmp $Blur | Out-Null }
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($full)) | Out-Null
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ($($bmp.Width)x$($bmp.Height)). Look at it before committing: no names, chat, paths or other people " +
  "(blur them: -Blur `"x,y,width,height`")."
$bmp.Dispose()
