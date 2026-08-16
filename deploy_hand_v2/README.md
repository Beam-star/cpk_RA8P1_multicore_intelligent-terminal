# v3 deploy (palm + stop 数据集)

面向瑞萨 EK-RA8P1（Ethos-U55 NPU）的 96x96 灰度**单类 hand** 检测部署包，
只包含 v3 学生的模型产物、测试脚本、转换流程和 RUHMI 编译结果。

本版使用 HAGRID palm + stop 合并数据集（`meeting_palm_stop_11k_mix`）重新训练，
训练时加入随机亮度/对比度增强（默认 0.3/0.3），epochs=150。
模型来源：`runs/detect/runs/detect/gesture_v3_w075_96_palm_stop/weights/best.pt`。

## 目录

```text
v3_deploy_palm_stop/
  model/
    gesture_v3.pt          PyTorch 权重
    gesture.onnx           FP32 ONNX
    gesture_int8.onnx      INT8 QDQ ONNX
    gesture_int8.tflite    全 INT8 TFLite
  scripts/
    export_onnx.py         .pt -> ONNX
    quantize_onnx_int8.py  ONNX -> INT8 QDQ
    convert_tflite.ps1     ONNX -> 全 INT8 TFLite
    compile_ruhmi.ps1      TFLite -> Ethos-U55 C 源码
    webcam_test.py         student .pt 测试
    webcam_onnx_test.py    INT8 ONNX 测试
    webcam_tflite_test.py  INT8 TFLite 测试
    validate_gesture.py    96 输入 predict 口径评测
  ruhmi_output/            RUHMI 编译产物
  DECODE.md                板端 TFLite 输出解码说明
```

## 本地测试

```powershell
D:\anaconda\envs\spk\python.exe face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\webcam_test.py
D:\anaconda\envs\spk\python.exe face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\webcam_onnx_test.py
D:\anaconda\envs\ra8_env\python.exe face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\webcam_tflite_test.py
```

默认 96x96 灰度、`--conf 0.45`。

## 重新导出

```powershell
D:\anaconda\envs\spk\python.exe face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\export_onnx.py
D:\anaconda\envs\spk\python.exe face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\quantize_onnx_int8.py
powershell -File face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\convert_tflite.ps1
```

## RUHMI 编译

```powershell
powershell -File face_detect\deploy_gesture\v3_deploy_palm_stop\scripts\compile_ruhmi.ps1
```

产物在 `ruhmi_output/gesture_int8_NPU/deploy/build/MCU/compilation/src`。

## e2 studio 集成

把 `ruhmi_output\gesture_int8_NPU\deploy\build\MCU\compilation\src` 下的
C 源码复制到工程 `src\ai_application\face_detection\mera`，输入改为 96x96
灰度，解码见 `DECODE.md`。


