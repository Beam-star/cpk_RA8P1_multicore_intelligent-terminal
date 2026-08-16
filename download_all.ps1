<#
  download_all.ps1 - Program all data to a fresh CPK board's W25Q256 flash.

  PREREQUISITE: the board must be running the MODE_PCDC_ECHO firmware
  (USB virtual COM port active). See cpu0main_thread_entry.c: OPERATING_MODE.

  Usage (from any directory):
    .\download_all.ps1 -Port COM10     # specify the COM port
    .\download_all.ps1                  # auto-detect Renesas PCDC (VID:045B PID:5001)

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
Flash info

Write-Host ""
Write-Host "=== 1) Face detection model (0x000000) ==="
Flash erase "0x000000" "0x070000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/ai_application/model/sub_0000_model_data.c"     "0x000000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/ai_application/model/sub_0000_command_stream.c" "0x0003D300"

Write-Host ""
Write-Host "=== 2) Asset directory (0x070000) ==="
Flash erase "0x070000" "0x1000"

Write-Host ""
Write-Host "=== 3) Boot logo (0x071000) ==="
Flash erase "0x071000" "0x12C000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/lvgl_ui/LOGO/boot_logo.bin" "0x071000"

Write-Host ""
Write-Host "=== 4) Animated mascot frames (line dog, 14 x 128x128) ==="
Flash erase "0x210000" "0x70000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/lvgl_ui/assets/dog_128_128.bin"  "0x210000"

Write-Host ""
Write-Host "=== 4b) Top-strip logo (640x120, above camera) ==="
Flash erase "0x1E0000" "0x25800"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/lvgl_ui/assets/toplogo_640_120.bin"  "0x1E0000"

Write-Host ""
Write-Host "=== 4c) Fingerprint loading spinner (20 x 96x96) ==="
Flash erase "0x290000" "0x5A000"
Flash write "cpk_RA8P1_multicore_IT_CPU0/src/lvgl_ui/assets/loading_96_96.bin"  "0x290000"

Write-Host ""
Write-Host "=== 5) Hand detection model (0xB00000 / 0xB40000) ==="
Flash erase "0x00B00000" "0x00040000"
Flash write "deploy_hand_v2/ruhmi_output/gesture_int8_NPU/deploy/build/MCU/compilation/src/sub_0000_model_data.c"     "0x00B00000"
Flash erase "0x00B40000" "0x00010000"
Flash write "deploy_hand_v2/ruhmi_output/gesture_int8_NPU/deploy/build/MCU/compilation/src/sub_0000_command_stream.c" "0x00B40000"

Write-Host ""
Write-Host "=== 6) Register assets in directory ==="
Flash dir_add "logo" "0x071000" "1228800"
Flash dir_add "toplogo" "0x1E0000" "153600"

Write-Host ""
Write-Host "All done. Remember to set OPERATING_MODE back to MODE_CAMERA and rebuild."
