#!/usr/bin/env python3
"""Validate the 96x96 gesture student at the exact deployment input size.

Ultralytics model.val() can pad square 96x96 images up to 128x128 when rect
mode is enabled, which evaluates the model at the wrong input size. This
script forces imgsz=96 and rect=False so validation matches deployment.
"""

import argparse
import os
import sys
from pathlib import Path

import numpy as np

PROJECT_DIR = Path(__file__).resolve().parent.parents[1]
os.environ.setdefault("YOLO_CONFIG_DIR", str(PROJECT_DIR / "Ultralytics"))
sys.path.insert(0, str(PROJECT_DIR))

from ultralytics import YOLO

DEFAULT_WEIGHTS = (
    PROJECT_DIR.parent
    / "runs/detect/runs/detect/gesture_v3_w075_96_palm_crop_mix/weights/best.pt"
)
DEFAULT_DATA = PROJECT_DIR.parent / "datasets" / "meeting_palm_crop_11k_mix.yaml"


def _box_iou(box, gt):
    x1, y1 = max(box[0], gt[0]), max(box[1], gt[1])
    x2, y2 = min(box[2], gt[2]), min(box[3], gt[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    area_a = (box[2] - box[0]) * (box[3] - box[1])
    area_b = (gt[2] - gt[0]) * (gt[3] - gt[1])
    return inter / max(area_a + area_b - inter, 1e-9)


def predict_recall(model, data_dir, conf, iou, max_det, imgsz, device):
    val_images = data_dir / "images" / "val"
    val_labels = data_dir / "labels" / "val"
    tp = fp = fn = 0
    for image_path in sorted(val_images.glob("*.jpg")):
        label_path = val_labels / f"{image_path.stem}.txt"
        if not label_path.is_file():
            continue
        gt_boxes = []
        for line in label_path.read_text().strip().splitlines():
            parts = line.split()
            if len(parts) < 5:
                continue
            cx, cy, bw, bh = map(float, parts[1:5])
            gt_boxes.append(np.array([
                (cx - bw / 2) * imgsz,
                (cy - bh / 2) * imgsz,
                (cx + bw / 2) * imgsz,
                (cy + bh / 2) * imgsz,
            ]))
        result = model.predict(
            str(image_path),
            conf=conf,
            iou=iou,
            imgsz=imgsz,
            max_det=max_det,
            device=device,
            verbose=False,
        )[0]
        preds = result.boxes.xyxy.cpu().numpy() if result.boxes is not None else np.zeros((0, 4))
        used = set()
        for gt in gt_boxes:
            best_index = -1
            best_iou = 0.0
            for index, pred in enumerate(preds):
                if index in used:
                    continue
                value = _box_iou(pred, gt)
                if value > best_iou:
                    best_iou = value
                    best_index = index
            if best_iou >= 0.5:
                tp += 1
                used.add(best_index)
            else:
                fn += 1
        fp += max(0, len(preds) - len(gt_boxes))
    return tp / max(tp + fn, 1), tp / max(tp + fp, 1), tp, fn, fp


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate the 96x96 gesture student.")
    parser.add_argument("--weights", default=str(DEFAULT_WEIGHTS))
    parser.add_argument("--data", default=str(DEFAULT_DATA))
    parser.add_argument("--imgsz", type=int, default=96)
    parser.add_argument("--conf", type=float, default=0.001)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--max-det", type=int, default=300)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--name", default="val96")
    parser.add_argument("--predict-conf", type=float, default=0.35,
                        help="Confidence used for the predict-based recall check.")
    parser.add_argument("--predict-max-det", type=int, default=1,
                        help="Max detections per image for the predict-based recall check.")
    args = parser.parse_args()

    model = YOLO(args.weights)
    results = model.val(
        data=args.data,
        imgsz=args.imgsz,
        rect=False,
        conf=args.conf,
        iou=args.iou,
        max_det=args.max_det,
        device=args.device,
        batch=args.batch,
        name=args.name,
        verbose=False,
    )
    print(
        f"96x96 val | P {results.box.mp:.4f} | R {results.box.mr:.4f} | "
        f"mAP50 {results.box.map50:.4f} | mAP50-95 {results.box.map:.4f}"
    )
    data_yaml = Path(args.data)
    data_dir = data_yaml.parent / data_yaml.stem if data_yaml.suffix == ".yaml" else data_yaml
    recall, precision, tp, fn, fp = predict_recall(
        model,
        data_dir,
        args.predict_conf,
        args.iou,
        args.predict_max_det,
        args.imgsz,
        args.device,
    )
    print(
        f"predict@{args.predict_conf} | P {precision:.4f} | R {recall:.4f} | "
        f"TP {tp} | FN {fn} | FP {fp}"
    )


if __name__ == "__main__":
    main()
