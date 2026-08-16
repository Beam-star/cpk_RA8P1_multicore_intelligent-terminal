#!/usr/bin/env python3
"""Local webcam/image test for the v3 96 grayscale gesture detector (.pt)."""

import argparse
import os
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(PROJECT_DIR))
os.environ.setdefault("YOLO_CONFIG_DIR", str(PROJECT_DIR / "Ultralytics"))

import cv2
import numpy as np
import torch

from ultralytics import YOLO
from ultralytics.utils.nms import non_max_suppression

DEFAULT_MODEL = (
    SCRIPT_DIR.parent.parents[1]
    / "runs/detect/runs/detect/gesture_v3_w075_96_palm_crop_mix2/weights/best.pt"
)
CLASS_NAMES = [
    "hand",
]


def pick_v3_model() -> Path:
    if DEFAULT_MODEL.is_file():
        return DEFAULT_MODEL
    candidates = sorted(
        PROJECT_DIR.parent.glob("runs/detect/runs/detect/gesture_v3*"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    for run_dir in candidates:
        best = run_dir / "weights" / "best.pt"
        if best.is_file():
            return best
    fallback = SCRIPT_DIR.parent / "model" / "gesture_v3.pt"
    if fallback.is_file():
        return fallback
    raise FileNotFoundError(
        "No v3 student checkpoint found. Train first or pass --model explicitly; "
        f"preferred: {DEFAULT_MODEL}"
    )


def resolve_device(device: str) -> str:
    if device in ("auto", "cpu"):
        return "cpu"
    if device.isdigit():
        return f"cuda:{device}"
    return device


def open_camera(index: int) -> cv2.VideoCapture:
    for backend in (cv2.CAP_DSHOW, cv2.CAP_MSMF, cv2.CAP_ANY):
        capture = cv2.VideoCapture(index, backend)
        if capture.isOpened():
            return capture
        capture.release()
    raise RuntimeError(f"Cannot open camera {index}.")


def box_iou(box_a: np.ndarray, box_b: np.ndarray) -> float:
    x1 = max(box_a[0], box_b[0])
    y1 = max(box_a[1], box_b[1])
    x2 = min(box_a[2], box_b[2])
    y2 = min(box_a[3], box_b[3])
    intersection = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    area_a = (box_a[2] - box_a[0]) * (box_a[3] - box_a[1])
    area_b = (box_b[2] - box_b[0]) * (box_b[3] - box_b[1])
    return intersection / max(area_a + area_b - intersection, 1e-9)


class TemporalTracker:
    def __init__(self, iou_threshold=0.35, max_miss=3, confirm_frames=2):
        self.iou_threshold = iou_threshold
        self.max_miss = max_miss
        self.confirm_frames = confirm_frames
        self.tracks = []

    def update(self, boxes, scores, class_ids):
        if not len(boxes):
            for track in self.tracks:
                track["missed"] += 1
        else:
            used_tracks = set()
            for box, score, class_id in zip(boxes, scores, class_ids):
                best_index = -1
                best_iou = self.iou_threshold
                for index, track in enumerate(self.tracks):
                    if index in used_tracks or track["missed"] > self.max_miss:
                        continue
                    iou = box_iou(box, track["box"])
                    if iou > best_iou:
                        best_iou = iou
                        best_index = index
                if best_index >= 0:
                    track = self.tracks[best_index]
                    track["box"] = 0.5 * track["box"] + 0.5 * box
                    track["score"] = max(score, track["score"] * 0.9)
                    track["class_id"] = class_id
                    track["hits"] += 1
                    track["missed"] = 0
                    used_tracks.add(best_index)
                else:
                    self.tracks.append({
                        "box": box.astype(np.float64).copy(),
                        "score": float(score),
                        "class_id": int(class_id),
                        "hits": 1,
                        "missed": 0,
                    })
            for index, track in enumerate(self.tracks):
                if index not in used_tracks:
                    track["missed"] += 1
        self.tracks = [track for track in self.tracks if track["missed"] <= self.max_miss]
        kept = [track for track in self.tracks if track["hits"] >= self.confirm_frames]
        if not kept:
            return np.zeros((0, 4)), np.zeros(0), np.zeros(0, dtype=int)
        boxes_out = np.asarray([track["box"] for track in kept])
        scores_out = np.asarray([track["score"] for track in kept])
        class_out = np.asarray([track["class_id"] for track in kept])
        order = np.argsort(scores_out)[::-1]
        return boxes_out[order], scores_out[order], class_out[order]


def predict_gray_frame(model, frame, size, conf, iou, max_det, device, max_area=0.9):
    if frame.ndim == 3:
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    else:
        gray = frame
    height, width = gray.shape[:2]
    ratio = min(size / height, size / width)
    new_width = max(1, round(width * ratio))
    new_height = max(1, round(height * ratio))
    resized = cv2.resize(gray, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((size, size), 114, dtype=np.uint8)
    offset_x = (size - new_width) // 2
    offset_y = (size - new_height) // 2
    canvas[offset_y:offset_y + new_height, offset_x:offset_x + new_width] = resized
    tensor = torch.from_numpy((canvas.astype(np.float32) / 255.0)[None, None]).to(device)
    model.model.eval()
    with torch.no_grad():
        predictions = model.model(tensor)
    detections = non_max_suppression(predictions, conf_thres=conf, iou_thres=iou, max_det=max_det)[0]
    if detections is None or not len(detections):
        return np.zeros((0, 4)), np.zeros(0), np.zeros(0, dtype=int)
    boxes = detections[:, :4].cpu().numpy()
    scores = detections[:, 4].cpu().numpy()
    class_ids = detections[:, 5].cpu().numpy().astype(int)
    areas = (boxes[:, 2] - boxes[:, 0]) * (boxes[:, 3] - boxes[:, 1]) / (size * size)
    keep = areas <= max_area
    boxes = boxes[keep]
    scores = scores[keep]
    class_ids = class_ids[keep]
    if not len(boxes):
        return np.zeros((0, 4)), np.zeros(0), np.zeros(0, dtype=int)
    boxes[:, [0, 2]] = (boxes[:, [0, 2]] - offset_x) / ratio
    boxes[:, [1, 3]] = (boxes[:, [1, 3]] - offset_y) / ratio
    return boxes, scores, class_ids


def draw_detections(display, boxes, scores, class_ids):
    colors = [
        (0, 220, 0), (220, 180, 0), (0, 160, 255), (0, 0, 230),
        (200, 80, 0), (255, 120, 120), (150, 150, 0), (0, 200, 200),
        (180, 0, 180), (60, 60, 220),
    ]
    for box, score, class_id in zip(boxes.astype(int), scores, class_ids):
        x1, y1, x2, y2 = box
        color = colors[int(class_id) % len(colors)]
        label = CLASS_NAMES[int(class_id)]
        cv2.rectangle(display, (x1, y1), (x2, y2), color, 2)
        cv2.putText(display, f"{label} {score:.2f}", (x1, max(20, y1 - 6)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2, cv2.LINE_AA)
    return display


def main():
    parser = argparse.ArgumentParser(description="Test the v3 96 gray gesture detector.")
    parser.add_argument("--model", default="")
    parser.add_argument("--source", default="0")
    parser.add_argument("--imgsz", type=int, default=96)
    parser.add_argument("--conf", type=float, default=0.45)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--max-det", type=int, default=1)
    parser.add_argument("--max-area", type=float, default=0.9, help="Reject boxes larger than this fraction of the model input.")
    parser.add_argument("--device", default="auto")
    parser.add_argument("--no-track", action="store_true")
    args = parser.parse_args()

    model_path = Path(args.model) if args.model else pick_v3_model()
    print(f"Using model: {model_path}")
    model = YOLO(model_path)
    device = resolve_device(args.device)
    model.model.to(device)

    source = int(args.source) if args.source.isdigit() else args.source
    if isinstance(source, int):
        capture = open_camera(source)
        previous_time = time.perf_counter()
        tracker = TemporalTracker()
        print("Camera started. Press q or Esc to exit.")
        try:
            while True:
                ok, frame = capture.read()
                if not ok:
                    break
                boxes, scores, class_ids = predict_gray_frame(
                    model, frame, args.imgsz, args.conf, args.iou, args.max_det, device, args.max_area
                )
                if not args.no_track:
                    boxes, scores, class_ids = tracker.update(boxes, scores, class_ids)
                display = draw_detections(frame.copy(), boxes, scores, class_ids)
                fps = 1.0 / max(time.perf_counter() - previous_time, 1e-6)
                previous_time = time.perf_counter()
                cv2.putText(display, f"v3 96 hand | {len(boxes)} det | {fps:.1f} FPS", (12, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 220, 0), 2, cv2.LINE_AA)
                cv2.imshow("RA8P1 v3 Gesture Detection", display)
                key = cv2.waitKey(1) & 0xFF
                if key in (ord("q"), 27):
                    break
        finally:
            capture.release()
            cv2.destroyAllWindows()
        return

    frame = cv2.imread(str(source))
    if frame is None:
        raise RuntimeError(f"Cannot read image: {source}")
    boxes, scores, class_ids = predict_gray_frame(
        model, frame, args.imgsz, args.conf, args.iou, args.max_det, device, args.max_area
    )
    display = draw_detections(frame, boxes, scores, class_ids)
    output_dir = SCRIPT_DIR.parent / "output"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / Path(source).name
    cv2.imwrite(str(output_path), display)
    print(f"{Path(source).name}: {len(boxes)} detections -> {output_path}")


if __name__ == "__main__":
    main()
