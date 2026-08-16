# UI Assets for Meeting Terminal

This directory holds raster images (RGB565 raw, no header) deployed to W25Q64
via the PCDC flash tool.  Images are loaded at boot via `lvgl_ui_assets.c` and
rendered as `lv_image` objects.

## Asset list

| # | Filename | Size (W×H) | B | Description | Needed? |
|---|----------|------------|---|-------------|---------|
| 1 | `logo_32.png` → `logo_32.bin` | 32×32 | 2048 | Title-bar logo (meeting/video icon) | 推荐 |
| 2 | `icon_enroll.png` → `icon_enroll.bin` | 24×24 | 1152 | "Enroll Face" button icon (person+) | 推荐 |
| 3 | `icon_checkin.png` → `icon_checkin.bin` | 24×24 | 1152 | "Check-in" button icon (checkmark) | 推荐 |
| 4 | `icon_record.png` → `icon_record.bin` | 24×24 | 1152 | "Start Rec" button icon (red circle) | 推荐 |
| 5 | `icon_stop.png` → `icon_stop.bin` | 24×24 | 1152 | "Stop Rec" button icon (square) | 推荐 |
| 6 | `icon_trash.png` → `icon_trash.bin` | 24×24 | 1152 | "Clear DB" button icon (trash) | 可选 |

Total: 5 recommended images, ~6.8 KB total.

## Format

- **Raw RGB565** (no BMP/PNG header), little-endian, same format as boot logo
- 2 bytes per pixel: `RRRRRGGGGGGBBBBB`

## Conversion (PC-side)

```bash
# Using ImageMagick:
magick icon.png -resize 24x24 -define bmp:subtype=RGB565 -depth 16 \
    BMP3:icon_tmp.bmp && tail -c +139 icon_tmp.bmp > icon.bin

# Or use the project's LVGLImage.py:
python scripts/LVGLImage.py input.png --format rgb565 --output-format bin
```

## Deployment

```bash
# In MODE_PCDC_ECHO, use pcdc_flash_tool.py:
python scripts/pcdc_flash_tool.py COM10 write logo_32.bin 0x0019D000
python scripts/pcdc_flash_tool.py COM10 write icon_enroll.bin 0x0019D800
# ... (offsets are assigned by the asset loader code)
```

## 纯代码备选方案

如果暂时没有图片资源, LVGL 可以纯代码画:
- 圆角矩形按钮 + 彩色渐变背景
- emoji/unicode 符号作为按钮图标 (📷 🎤 ⏺ ⏹ 🗑)
- 彩色状态指示点 (lv_obj + lv_style + 圆形)
- 卡片式分区 (嵌套容器 + 阴影)
