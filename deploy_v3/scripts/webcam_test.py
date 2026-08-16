#!/usr/bin/env python3
"""Local webcam/image test for the w05 96 grayscale face detector.

Standalone copy: loads the packaged best.pt, runs 80x80 grayscale inference,
applies face-like box filtering, frame-to-frame smoothing and NMS.
"""

import argparse
import os
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR.parents[1]))
os.environ.setdefault("YOLO_CONFIG_DIR", str(SCRIPT_DIR.parents[1] / "Ultralytics"))

import cv2
import numpy as np
import torch

from ultralytics import YOLO
from ultralytics.utils.nms import non_max_suppression

DEFAULT_MODEL = SCRIPT_DIR.parent / "model" / "face.pt"


def resolve_device(device: str) -> str:
    if device in ("auto", "cpu"):
        return "cpu"
    if device.isdigit():
        return f"cuda:{device}"
    return device


def open_camera(index: int) -> cv2.VideoCapture:
    backends = [cv2.CAP_DSHOW, cv2.CAP_MSMF, cv2.CAP_ANY]
    for backend in backends:
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

    def update(self, boxes, scores):
        if not len(boxes):
            for track in self.tracks:
                track["missed"] += 1
        else:
            used_tracks = set()
            for box, score in zip(boxes, scores):
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
                    track["hits"] += 1
                    track["missed"] = 0
                    used_tracks.add(best_index)
                else:
                    self.tracks.append({
                        "box": box.astype(np.float64).copy(),
                        "score": float(score),
                        "hits": 1,
                        "missed": 0,
                    })
            for index, track in enumerate(self.tracks):
                if index not in used_tracks:
                    track["missed"] += 1
        self.tracks = [track for track in self.tracks if track["missed"] <= self.max_miss]
        kept = [track for track in self.tracks if track["hits"] >= self.confirm_frames]
        if not kept:
            return np.zeros((0, 4)), np.zeros(0)
        boxes_out = np.asarray([track["box"] for track in kept])
        scores_out = np.asarray([track["score"] for track in kept])
        order = np.argsort(scores_out)[::-1]
        return boxes_out[order], scores_out[order]


def predict_gray_frame(model, frame, size, conf, iou, max_det, device, conf_high=0.58):
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
    detections = non_max_suppression(predictions, conf_thres=conf, iou_thres=iou,
                                     max_det=max_det, classes=[0])[0]
    boxes = detections[:, :4].cpu().numpy() if detections is not None and len(detections) else np.zeros((0, 4))
    scores = detections[:, 4].cpu().numpy() if detections is not None and len(detections) else np.zeros(0)
    boxes[:, [0, 2]] = (boxes[:, [0, 2]] - offset_x) / ratio
    boxes[:, [1, 3]] = (boxes[:, [1, 3]] - offset_y) / ratio
    if len(boxes):
        box_widths = boxes[:, 2] - boxes[:, 0]
        box_heights = boxes[:, 3] - boxes[:, 1]
        aspects = box_widths / np.maximum(box_heights, 1.0)
        areas = box_widths * box_heights / (width * height)
        size_ok = (box_widths <= width * 0.95) & (box_heights <= height * 0.95) & (areas >= 0.005)
        high_ok = (scores >= conf_high) | ((aspects >= 0.55) & (aspects <= 1.8))
        valid = size_ok & high_ok & (aspects >= 0.4) & (aspects <= 2.5)
        boxes = boxes[valid]
        scores = scores[valid]
    return boxes, scores


def draw_detections(display, boxes, scores):
    for person_id, (box, score) in enumerate(zip(boxes.astype(int), scores), 1):
        x1, y1, x2, y2 = box
        center_x = (x1 + x2) // 2
        center_y = (y1 + y2) // 2
        cv2.rectangle(display, (x1, y1), (x2, y2), (0, 220, 0), 2)
        cv2.putText(display, f"person{person_id} ({center_x},{center_y})", (x1, max(20, y1 - 6)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 220, 0), 2, cv2.LINE_AA)
    return display


def main():
    parser = argparse.ArgumentParser(description="Test the w05 96 grayscale face detector.")
    parser.add_argument("--model", default=str(DEFAULT_MODEL))
    parser.add_argument("--source", default="0")
    parser.add_argument("--imgsz", type=int, default=96)
    parser.add_argument("--conf", type=float, default=0.45)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--max-det", type=int, default=20)
    parser.add_argument("--conf-high", type=float, default=0.58)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--no-track", action="store_true")
    args = parser.parse_args()

    model = YOLO(args.model)
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
                boxes, scores = predict_gray_frame(model, frame, args.imgsz, args.conf,
                                                   args.iou, args.max_det, device, args.conf_high)
                if not args.no_track:
                    boxes, scores = tracker.update(boxes, scores)
                display = draw_detections(frame.copy(), boxes, scores)
                fps = 1.0 / max(time.perf_counter() - previous_time, 1e-6)
                previous_time = time.perf_counter()
                cv2.putText(display, f"w05 96 gray | faces {len(boxes)} | {fps:.1f} FPS", (12, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.65, (0, 220, 0), 2, cv2.LINE_AA)
                cv2.imshow("RA8P1 w05 96 Face Detection", display)
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
    boxes, scores = predict_gray_frame(model, frame, args.imgsz, args.conf,
                                       args.iou, args.max_det, device, args.conf_high)
    display = draw_detections(frame, boxes, scores)
    output_dir = SCRIPT_DIR.parent / "output"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / Path(source).name
    cv2.imwrite(str(output_path), display)
    print(f"{Path(source).name}: {len(boxes)} faces -> {output_path}")


if __name__ == "__main__":
    main()
