# =====================================================================
#  probe-glass-dcomp.ps1 -- does a PILL-SIZED DirectComposition window get a real blurred backdrop?
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\probe-glass-dcomp.ps1
#
#  WHY THIS EXISTS (HANDOFF 8.165/8.166)
#
#  The bar is a full-width per-pixel-alpha window, and the one material that blurs -- acrylic -- blurs
#  the WHOLE window rectangle and ignores the window region. So it was parked. What was never tried is
#  a window whose RECTANGLE is the pill: then there is no band to leak into, and the only question left
#  is whether the blur stops at the rounded corners.
#
#  The first version of tools\glassprobe used an ORDINARY window and answered nothing: the material is
#  drawn BEHIND the window's content, so an opaque window hides it completely (measured: the --plain and
#  acrylic photographs were pixel-for-pixel identical). The bar's own window is the per-pixel-alpha kind
#  (WS_EX_NOREDIRECTIONBITMAP + DirectComposition), so glassprobe --dcomp repeats the experiment there.
#
#  WHAT IS MEASURED
#
#    * edge energy inside the pill, with a black/white 4 px stripe pattern directly behind it: a real
#      blur collapses it, a tint alone does not;
#    * four photographs at 1x and 4x (the corner), so the shape can be looked at;
#    * --region, because the region did not clip acrylic on the bar's own window and it is worth knowing
#      whether that changes when the window is the pill;
#    * a 220x60 window, to see whether DWM's rounded-corner radius scales with the window or is fixed.
#
#  The stripe window is a PowerShell WinForms window on purpose: no extra binary, and it is BEHIND the
#  probe (which is topmost), so the probe sees a high-contrast backdrop rather than the desktop.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$Captures = Join-Path $Root 'captures'
if (-not (Test-Path $Captures)) { New-Item -ItemType Directory -Force -Path $Captures | Out-Null }

$Built = Join-Path $Root 'build\Debug\tools\glassprobe\Debug\glassprobe.exe'
if (-not (Test-Path $Built)) { "STOP: $Built is not built. Build the glassprobe target first."; exit 2 }

# Outside the project tree: an image under C:\PROJECT\Taskbar is launched at LOW integrity, and probes of
# this kind have been quietly affected by that before (START-HERE.txt).
$Exe = Join-Path $env:TEMP 'glassprobe-dcomp.exe'
Copy-Item -LiteralPath $Built -Destination $Exe -Force

# The stripe plane. 4 px white / 4 px black, which is far above what any blur can preserve.
$Plane = New-Object System.Windows.Forms.Form
$Plane.FormBorderStyle = 'None'
$Plane.StartPosition = 'Manual'
$Plane.Location = New-Object System.Drawing.Point 1130, 260
$Plane.Size = New-Object System.Drawing.Size 320, 140
$Plane.TopMost = $false
$Plane.ShowInTaskbar = $false
$Plane.add_Paint({
    param($sender, $eventArgs)
    $graphics = $eventArgs.Graphics
    $graphics.Clear([System.Drawing.Color]::Black)
    $white = [System.Drawing.Brushes]::White
    for ($x = 0; $x -lt 320; $x += 8) { $graphics.FillRectangle($white, $x, 0, 4, 140) }
})
$Plane.Show()
$Plane.Refresh()
Start-Sleep -Milliseconds 400

function Save-Shot([string]$Name, [int]$X, [int]$Y, [int]$W, [int]$H, [int]$Zoom) {
    $bitmap = New-Object System.Drawing.Bitmap $W, $H
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H))
    $graphics.Dispose()

    if ($Zoom -gt 1) {
        $large = New-Object System.Drawing.Bitmap ($W * $Zoom), ($H * $Zoom)
        $largeGraphics = [System.Drawing.Graphics]::FromImage($large)
        $largeGraphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $largeGraphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $largeGraphics.DrawImage($bitmap, 0, 0, $W * $Zoom, $H * $Zoom)
        $largeGraphics.Dispose()
        $large.Save((Join-Path $Captures $Name), [System.Drawing.Imaging.ImageFormat]::Png)
        $large.Dispose()
    } else {
        $bitmap.Save((Join-Path $Captures $Name), [System.Drawing.Imaging.ImageFormat]::Png)
    }
    $bitmap.Dispose()
}

# Mean |L(x) - L(x+1)| over the pill's interior in a saved photograph. The crop origin is 1160,290 and
# the window starts at 1180,300, so the pill's interior is x 28..232, y 16..40 in the crop.
function Get-EdgeEnergy([string]$Path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    $sum = 0.0
    $count = 0
    for ($y = 16; $y -lt 40; $y++) {
        for ($x = 28; $x -lt 232; $x++) {
            $left = $bitmap.GetPixel($x, $y)
            $right = $bitmap.GetPixel($x + 1, $y)
            $sum += [math]::Abs(($left.R + $left.G + $left.B) / 3 - ($right.R + $right.G + $right.B) / 3)
            $count++
        }
    }
    $bitmap.Dispose()
    if ($count -eq 0) { return 0.0 }
    return [math]::Round($sum / $count, 2)
}

function Invoke-Probe([string]$Name, [string]$Arguments, [int]$CropW, [int]$CropH) {
    $out = Join-Path $Captures ($Name + '.log')
    $err = Join-Path $Captures ($Name + '.err')
    Remove-Item -LiteralPath $out, $err -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru -WindowStyle Hidden `
                             -RedirectStandardOutput $out -RedirectStandardError $err
    Start-Sleep -Milliseconds 1400
    Save-Shot ($Name + '.png') 1160 290 $CropW $CropH 1
    Save-Shot ($Name + '-corner.png') 1160 290 56 56 4
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    Start-Sleep -Milliseconds 250
    "  $Name : edge energy = " + (Get-EdgeEnergy (Join-Path $Captures ($Name + '.png')))
}

"probe: pill-sized DirectComposition window, 220x36 at 1180,300, over 4 px stripes"
"--- no material (control) ---"
Invoke-Probe 'dcomp-stripes-plain' '--dcomp --plain --at 1180,300 --size 220x36 --seconds 10' 260 76
"--- acrylic ---"
Invoke-Probe 'dcomp-stripes-acrylic' '--dcomp --at 1180,300 --size 220x36 --seconds 10' 260 76
"--- acrylic + a stadium window REGION (r = 18) ---"
Invoke-Probe 'dcomp-region' '--dcomp --region --at 1180,300 --size 220x36 --seconds 10' 260 76
"--- acrylic, 60 tall, to see whether the corner radius scales ---"
Invoke-Probe 'dcomp-tall' '--dcomp --at 1180,300 --size 220x60 --seconds 10' 260 100

$Plane.Close()

"--- the probe's own account of the acrylic run ---"
Get-Content (Join-Path $Captures 'dcomp-stripes-acrylic.log') | Select-String 'material=|acrylic ->|CORNER|window '
'done'
