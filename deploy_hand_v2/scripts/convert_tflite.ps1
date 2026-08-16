$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$deployRoot = Split-Path -Parent $scriptDir
$onnx = Join-Path $deployRoot "model\gesture.onnx"
$work = Join-Path $deployRoot "tflite_work"
$ra8Python = "D:\anaconda\envs\ra8_env\python.exe"
$calibNpy = "C:\Users\19510\Desktop\work_space\renesas\ra8p1_work\face_detect\.codex_tmp\calib_hand96.npy"

if (-not (Test-Path -LiteralPath $onnx)) {
    throw "ONNX not found: $onnx"
}
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Split the graph at the two raw per-anchor outputs so the TFLite model outputs
# box logits [1,4,180] and class logits [1,1,180]. Decode (softplus/sigmoid +
# anchor mapping + NMS) runs on the board CPU, matching the official YOLO-Fastest
# TFLite workflow.
& $ra8Python -c @"
import onnx
from pathlib import Path
src = Path(r'$onnx')
dst = Path(r'$work\gesture_rawlogits.onnx')
m = onnx.load(str(src))
m = onnx.shape_inference.infer_shapes(m)
keep = {'/wrapped/model.24/Concat_output_0', '/wrapped/model.24/Concat_1_output_0'}
value_infos = {vi.name: vi for vi in m.graph.value_info}
seen = set()
output_infos = []
for node in m.graph.node:
    for name in node.output:
        if name in keep and name not in seen:
            info = onnx.helper.ValueInfoProto()
            info.CopyFrom(value_infos[name])
            output_infos.append(info)
            seen.add(name)
del m.graph.output[:]
for info in output_infos:
    m.graph.output.append(info)
onnx.save(m, str(dst))
print('preconcat onnx ->', dst, 'outputs:', sorted(keep))
"@

& $ra8Python -m onnxsim (Join-Path $work "gesture_rawlogits.onnx") $work\gesture_rawlogits_sim.onnx

& $ra8Python -m onnx2tf `
    -i $work\gesture_rawlogits_sim.onnx `
    -o $work\gesture_tflite `
    -oiqt `
    -qt per-channel `
    -cind images $calibNpy 0 1

$final = Join-Path $deployRoot "model\gesture_int8.tflite"
$converted = Get-ChildItem -Path (Join-Path $work "gesture_tflite") -Filter "*full_integer_quant.tflite" | Select-Object -First 1
Copy-Item -Force $converted.FullName $final
Write-Output "Full INT8 TFLite saved: $final"

