# v3 TFLite 板端输出解码说明

本文档只说明上板模型 `face_int8.tflite` 在板端的输出含义和解码流程。
v1/v2/v3 结构相同，解码逻辑完全一致。

## 1. TFLite 输出

模型只输出原始 logits，解码全部在板端 CPU 完成：

```text
输出 1: [1, 4, 180]   box logits
输出 2: [1, 1, 180]   score logits
```

两个输出都是 Int8，使用各自的 scale / zero_point 反量化。

## 2. 180 个候选的排列

180 = 144 + 36，顺序是先 stride 8，再 stride 16：

| stride | 网格 | 格子数 | 起始下标 |
| --- | --- | --- | --- |
| 8 | 12x12 | 144 | 0 |
| 16 | 6x6 | 36 | 144 |

每个 scale 内部按行优先排列：

```text
col = index % grid
row = index // grid
格心 = (col + 0.5, row + 0.5)
```

## 3. 板端解码步骤

### 3.1 反量化

```text
real = (int8_value - zero_point) * scale
```

### 3.2 距离和置信度

```text
l, t, r, b = softplus(box_logits[0:4, i])
score      = sigmoid(score_logits[0, i])
```

softplus 公式：

```text
softplus(x) = log(1 + exp(x))
```

### 3.3 转成 96 输入空间坐标

`l/t/r/b` 是格心单位下的距离，最后乘 stride 得到输入像素：

```text
cx = (grid_x + (r - l) / 2) * stride
cy = (grid_y + (b - t) / 2) * stride
w  = (l + r) * stride
h  = (t + b) * stride
```

等价写法：

```text
x1 = (grid_x - l) * stride
y1 = (grid_y - t) * stride
x2 = (grid_x + r) * stride
y2 = (grid_y + b) * stride
```

### 3.4 NMS

```text
置信度阈值：0.45
IoU 阈值：0.45
最大框数：20
```

NMS 前先把 `xywh` 转成 `xyxy`，再按置信度降序做标准 NMS。

## 4. 坐标还原到原图

模型输出坐标都在 96x96 letterbox 输入空间，还原公式：

```text
ratio = min(96 / 原图高, 96 / 原图宽)
new_w = round(原图宽 * ratio)
new_h = round(原图高 * ratio)
offset_x = (96 - new_w) // 2
offset_y = (96 - new_h) // 2

原图_x = (96空间_x - offset_x) / ratio
原图_y = (96空间_y - offset_y) / ratio
```

## 5. Python 参考实现

对应 `scripts/webcam_tflite_test.py` 里的核心逻辑：

```python
import numpy as np

def decode_logits(raw_box, raw_score):
    dist = np.log1p(np.exp(np.clip(raw_box[0], -80.0, 80.0)))
    scores = 1.0 / (1.0 + np.exp(-np.clip(raw_score[0, 0], -80.0, 80.0)))
    boxes = []
    confs = []
    for scale_index, (stride, grid) in enumerate([(8, 12), (16, 6)]):
        count = grid * grid
        start = 0 if scale_index == 0 else 144
        for index in range(count):
            column = index % grid
            row = index // grid
            left, top, right, bottom = dist[:, start + index]
            cx = ((column + 0.5 - left) + (column + 0.5 + right)) / 2 * stride
            cy = ((row + 0.5 - top) + (row + 0.5 + bottom)) / 2 * stride
            w = (right + left) * stride
            h = (bottom + top) * stride
            boxes.append([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2])
            confs.append(float(scores[start + index]))
    return np.asarray(boxes, dtype=np.float32).reshape(-1, 4), np.asarray(confs, dtype=np.float32)
```

反量化示例：

```python
scale, zero_point = output_detail["quantization"]
values = (interpreter.get_tensor(output_detail["index"]).astype(np.float32) - zero_point) * scale
```

## 6. 常见错误

1. TFLite 输出是 softplus/sigmoid **之前**的 logits，不能直接用距离或分数，
   必须先激活再解码。
2. 两个 scale 的格子顺序不能颠倒：前 144 个是 stride 8，后 36 个是 stride 16。
3. Int8 输出必须先按 scale / zero_point 反量化，再做任何数学运算。
4. 解码后的框仍在 96x96 letterbox 空间，映射到原图前必须先还原 offset 和
   ratio。
