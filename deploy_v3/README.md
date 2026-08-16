# face deploy v3

面向瑞萨 EK-RA8P1（Ethos-U55 NPU）的 96x96 灰度会议室人脸检测模型，通过
RUHMI 框架编译部署。

v3 在 v2 基础上提高模型容量：主干宽度从 0.5 提升到 0.75，保留 SPPF，检测头
中间通道从 24 加宽到 32，换取更高精度。输入尺寸仍为 96x96 灰度，算力约
30M MACs，仍低于官方模型的 38.9M。

## 模型

| 项目 | 数值 |
| --- | --- |
| 输入 | 96x96 灰度 |
| 结构 | MobileNetV2 风格主干（width 0.75）+ SPPF + 轻量检测头（mid=32，reg_max=1，无 DFL） |
| 参数量 | 191,334（fused） |
| 算力 | 约 30.06 MMACs |
| FP32 best.pt AP50 / mAP50-95 | 0.923 / 0.638 |
| FP32 ONNX AP50 / mAP50-95 | 0.923 / 0.640 |
| INT8 ONNX AP50 / mAP50-95 | 0.923 / 0.621 |

## 文件

```text
deploy_v3/
  model/
    face.pt              训练好的权重
    face.yaml            网络结构配置
    face.onnx            FP32 ONNX
    face_int8.onnx       INT8 QDQ ONNX
    face_int8.tflite     全 INT8 TFLite（官方同款部署格式）
  scripts/
    webcam_test.py       PyTorch .pt 本地摄像头/图片测试
    webcam_onnx_test.py  INT8 ONNX 本地摄像头/图片测试
    webcam_tflite_test.py 全 INT8 TFLite 本地摄像头/图片测试
    export_onnx.py       face.pt -> face.onnx
    quantize_onnx_int8.py 校准量化 -> face_int8.onnx
    train_distill_gray_v3.py v3 训练入口（teacher 为 kd3 best.pt）
    compile_ruhmi.ps1    RUHMI mcu_compile.py 封装脚本
  ruhmi_output/          MERA/Vela 生成的 C 源码
  DECODE.md              板端 TFLite 输出解码说明
```

## 本地测试

```powershell
python scripts\webcam_test.py
python scripts\webcam_onnx_test.py
D:\anaconda\envs\ra8_env\python.exe scripts\webcam_tflite_test.py
```

默认 96x96 灰度、置信度 0.45，按 q 或 Esc 退出。摄像头脚本已带帧间平滑，
可用 `--no-track` 关闭。

## 导出与量化

```powershell
python scripts\export_onnx.py
python scripts\quantize_onnx_int8.py
```

量化脚本会自动定位检测头模块，把解码算子留在 FP32，避免框/分数合并张量在
INT8 下把分数压成 0。

## RUHMI 编译

```powershell
powershell -File scripts\compile_ruhmi.ps1
```

当前 `model\face_int8.tflite` 编译结果：

| 指标 | 数值 |
| --- | --- |
| NPU 覆盖 | 100%（130 ops，0 CPU 回退） |
| Tensor Arena RAM | 221,184 bytes（216.00 KiB） |
| Flash 参数 | 250,416 bytes（244.55 KiB） |
| MACs | 30,056,256 |
| 输入 | Int8 `[1,96,96,1]` |
| 输出 | Int8 `[1,4,180]` + `[1,1,180]` |

## 版本对比

| 版本 | 参数量 | MACs | TFLite 大小 | TFLite @0.45 TP/FP | P | R |
| --- | --- | --- | --- | --- | --- | --- |
| v1 | 86,074 | 13.75M | 150 KB | 119/13 | 0.902 | 0.688 |
| v2 | 94,514 | 14.33M | 166 KB | 136/12 | 0.919 | 0.786 |
| v3 | 191,334 | 30.06M | 280 KB | 152/10 | 0.938 | 0.879 |

与官方模型对比：

| 项目 | 官方 yolo-fastest_192_face_v4 | v3 |
| --- | --- | --- |
| 输入 | 192x192 灰度 INT8 | 96x96 灰度 INT8 |
| 参数量 | 约 28.1 万 | 191,334 |
| MACs | 38.92M | 30.06M |
| 模型体积 | 510.3 KB TFLite | 280.1 KB TFLite |
| 验证集 AP50 | 0.877（官方 tflite 自解码，conf=0.5） | 0.923（INT8 ONNX，Ultralytics 口径） |

说明：官方 AP50 是它自己的 TFLite 解码 + conf=0.5 口径，v3 是 Ultralytics
口径，不能严格直接比较，但都是同一个验证集（161 张 / 173 个脸）。

## 理论帧率

Ethos-U55：500 MHz，256 MAC/cycle，峰值算力 128 GMAC/s。

```text
单帧 NPU 时间 = MACs / (256 × 500 MHz)
```

| 模型 | 纯 NPU 算力上界 | 按官方 4ms 口径同比例 |
| --- | --- | --- |
| 官方 192 | 38.92M / 128 GMAC/s ≈ 0.30 ms | 4 ms（官方示例基准） |
| v3 96 | 30.06M / 128 GMAC/s ≈ 0.23 ms | 约 3.1 ms |

v3 的算力需求约为官方的 77%。加上 M85 上的灰度转换、缩放、CPU 解码和 NMS，
整条链路预计 5-8 ms 级别，30-60 FPS 实时没有问题。最终以
`sub_0000_model_data` 的大小和上板实测帧率为准。

## e2 studio 集成

1. 打开 `ruhmi-framework-mcu-main\...\application_examples\face_detection`。
2. 把 `ruhmi_output\face_int8_NPU\deploy\build\MCU\compilation\src` 下的生成
   源码复制到 `src\ai_application\face_detection\mera`。
3. 摄像头输入改为 96x96 灰度。检测头为无 DFL 结构：每个 anchor 输出 4 个框
   距离（softplus）+ 1 个分数，解码放在 CPU。
4. 核对 `sub_0000_model_data` 大小、Tensor Arena 和板上实测帧率。
