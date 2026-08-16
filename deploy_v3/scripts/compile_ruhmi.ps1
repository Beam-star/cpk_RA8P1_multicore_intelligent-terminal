$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$deployRoot = Split-Path -Parent $scriptDir
$onnx = Join-Path $deployRoot "model\face_int8.tflite"
$output = Join-Path $deployRoot "ruhmi_output"
$framework = "C:\Users\19510\Desktop\work_space\renesas\ra8p1_work\face_detect\ruhmi-framework-mcu-main\ruhmi-framework-mcu-main\scripts"
$ruhmiPython = "D:\anaconda\envs\ra8_env\python.exe"

if (-not (Test-Path -LiteralPath $onnx)) {
    throw "TFLite not found: $onnx. Run the ONNX->TFLite conversion first."
}

$env:PYTHONIOENCODING = "utf-8"
& $ruhmiPython (Join-Path $framework "mcu_compile.py") `
    $onnx `
    $output `
    --npu `
    --optimization Performance

Write-Output "RUHMI compile finished. Output: $output"
Write-Output "e2 studio integration source: $output\...\build\MCU\compilation\src"
