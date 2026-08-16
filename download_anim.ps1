<#
  download_anim.ps1 - Program only the two UI animation frame sets to W25Q256:
    - Animated mascot (line dog, 14 x 128x128 RGB565) @0x210000
    - Fingerprint loading spinner (20 x 96x96 RGB565)        @0x290000

  Use this when ONLY the GIFs changed and the rest of the flash (models,
  boot logo, top logo, font) is already programmed — much faster than
  re-running download_all.ps1.

  PREREQUISITE: the board must be running MODE_PCDC_ECHO firmware
  (USB virtual COM port active). See cpu0main_thread_entry.c: OPERATING_MODE.

  Usage:
    .\download_anim.ps1                  # auto-detect Renesas PCDC (VID:045B PID:5001)
    .\download_anim.ps1 -Port COM10      # specify the COM port

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

Write-Host "=== 0) Connectivity check ==="
Flash ping

Write-Host ""
Write-Host "=== 1) Animated mascot frames (line dog, 14 x 128x128) ==="
Flash erase "0x210000" "0x70000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/lvgl_ui/assets/dog_128_128.bin"  "0x210000"

Write-Host ""
Write-Host "=== 2) Fingerprint loading spinner (20 x 96x96) ==="
Flash erase "0x290000" "0x5A000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/lvgl_ui/assets/loading_96_96.bin"  "0x290000"

Write-Host ""
Write-Host "Animation frames done. Set OPERATING_MODE back to MODE_CAMERA and rebuild."
