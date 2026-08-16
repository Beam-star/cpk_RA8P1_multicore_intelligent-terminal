<#
  download_font.ps1 - Program the custom Chinese font (myChineseFont) to W25Q256.

  The font data was filtered from myChineseFont.c by scripts/filter_font.py
  (kept GB2312 L1 + ASCII, dropped L2 rare hanzi, bitmap < 1MB) into:
    scripts/font_bitmap.bin   (4bpp glyph bitmaps,  @0xBE0000)
    scripts/font_unicode.bin  (unicode_list_1,      @0xD11000)
    scripts/font_dsc.bin      (glyph_dsc[],         @0xD15000)

  PREREQUISITE: the board must be running the MODE_PCDC_ECHO firmware
  (USB virtual COM port active).

  Usage (from any directory):
    .\download_font.ps1 -Port COM10     # specify the COM port
    .\download_font.ps1                  # auto-detect Renesas PCDC (VID:045B PID:5001)

  If `python` is not on PATH, change $py below to `py`.
#>
[CmdletBinding()]
param(
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$py = "python"

function Flash {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$ToolArgs
    )
    $allArgs = @("scripts/pcdc_flash_tool.py")
    if ($Port) { $allArgs += $Port }
    $allArgs += $ToolArgs
    & $py @allArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed: $($ToolArgs -join ' ')"
    }
}

Write-Host "=== Connectivity check ==="
Flash ping

Write-Host ""
Write-Host "=== 1) Erase font region (0xBE0000, 1.375MB) ==="
Flash erase "0xBE0000" "0x150000"

Write-Host ""
Write-Host "=== 2) Write font data ==="
Flash write "scripts/font_bitmap.bin"  "0xBE0000"
Flash write "scripts/font_unicode.bin" "0xD11000"
Flash write "scripts/font_dsc.bin"     "0xD15000"

Write-Host ""
Write-Host "Chinese font download complete. Rebuild CPU0 (with built-in CJK fonts disabled) and flash."
