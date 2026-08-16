# TFLite 板端输出解码说明

## 1. TFLite 输出

```text
输出 1: [1, 4, 180]   box logits（softplus 后为 l/t/r/b 距离）
输出 2: [1, 1, 180]   class logits（sigmoid 后为 hand 分数）
```

## 2. 180 个候选的排列

180 = 144 + 36，顺序是先 stride 8，再 stride 16：

| stride | 网格 | 格子数 | 起始下标 |
| --- | --- | --- | --- |
| 8 | 12x12 | 144 | 0 |
| 16 | 6x6 | 36 | 144 |

## 3. 解码

```text
real = (int8_value - zero_point) * scale
l, t, r, b = softplus(box_logits[0:4, i])
score     = sigmoid(class_logits[0, i])

cx = (grid_x + (r - l) / 2) * stride
cy = (grid_y + (b - t) / 2) * stride
w  = (l + r) * stride
h  = (t + b) * stride
```

## 4. 坐标还原

```text
ratio = min(96 / 原图高, 96 / 原图宽)
new_w = round(原图宽 * ratio)
new_h = round(原图高 * ratio)
offset_x = (96 - new_w) // 2
offset_y = (96 - new_h) // 2

原图_x = (96空间_x - offset_x) / ratio
原图_y = (96空间_y - offset_y) / ratio
```

## 5. 常见错误

1. TFLite 输出是 softplus/sigmoid 之前的 logits，必须先激活。
2. 前 144 个是 stride 8，后 36 个是 stride 16。
3. Int8 输出必须先反量化再计算。
