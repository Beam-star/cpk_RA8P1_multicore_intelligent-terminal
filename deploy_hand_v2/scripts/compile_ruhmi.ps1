$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$deployRoot = Split-Path -Parent $scriptDir
$tflite = Join-Path $deployRoot "model\gesture_int8.tflite"
$output = Join-Path $deployRoot "ruhmi_output"
$framework = "C:\Users\19510\Desktop\work_space\renesas\ra8p1_work\face_detect\ruhmi-framework-mcu-main\ruhmi-framework-mcu-main\scripts"
$ruhmiPython = "D:\anaconda\envs\ra8_env\python.exe"

if (-not (Test-Path -LiteralPath $tflite)) {
    throw "TFLite not found: $tflite. Run convert_tflite.ps1 first."
}

$env:PYTHONIOENCODING = "utf-8"
& $ruhmiPython (Join-Path $framework "mcu_compile.py") `
    $tflite `
    $output `
    --npu `
    --optimization Performance

Write-Output "RUHMI compile finished. Output: $output"
Write-Output "e2 studio integration source: $output\...\build\MCU\compilation\src"
