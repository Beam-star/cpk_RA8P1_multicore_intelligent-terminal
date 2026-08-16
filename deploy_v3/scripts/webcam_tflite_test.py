#!/usr/bin/env python3
"""Local webcam/image test for the w05 96 full-INT8 TFLite face detector.

The TFLite model outputs raw box/score logits; decode (softplus + sigmoid +
anchor boxes + NMS) runs on the host CPU, matching the official YOLO-Fastest
TFLite workflow.
"""

import argparse
import time
from pathlib import Path

import cv2
import numpy as np
from ai_edge_litert.interpreter import Interpreter

SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_MODEL = SCRIPT_DIR.parent / "model" / "face_int8.tflite"
INPUT_SIZE = 96


def preprocess_gray(frame: np.ndarray):
    if frame.ndim == 3:
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    else:
        gray = frame
    height, width = gray.shape[:2]
    ratio = min(INPUT_SIZE / height, INPUT_SIZE / width)
    new_width = max(1, round(width * ratio))
    new_height = max(1, round(height * ratio))
    resized = cv2.resize(gray, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((INPUT_SIZE, INPUT_SIZE), 114, dtype=np.uint8)
    offset_x = (INPUT_SIZE - new_width) // 2
    offset_y = (INPUT_SIZE - new_height) // 2
    canvas[offset_y:offset_y + new_height, offset_x:offset_x + new_width] = resized
    return canvas, ratio, offset_x, offset_y


def decode_logits(raw_box: np.ndarray, raw_score: np.ndarray):
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
            center_x = ((column + 0.5 - left) + (column + 0.5 + right)) / 2 * stride
            center_y = ((row + 0.5 - top) + (row + 0.5 + bottom)) / 2 * stride
            box_width = (right + left) * stride
            box_height = (bottom + top) * stride
            boxes.append([
                center_x - box_width / 2,
                center_y - box_height / 2,
                center_x + box_width / 2,
                center_y + box_height / 2,
            ])
            confs.append(float(scores[start + index]))
    return np.asarray(boxes, dtype=np.float32).reshape(-1, 4), np.asarray(confs, dtype=np.float32)


def run_model(interpreter, canvas: np.ndarray):
    input_detail = interpreter.get_input_details()[0]
    output_details = interpreter.get_output_details()
    scale, zero_point = input_detail["quantization"]
    quantized = np.round(canvas.astype(np.float32) / 255.0 / scale + zero_point).astype(input_detail["dtype"])
    interpreter.set_tensor(input_detail["index"], quantized[None, ..., None])
    interpreter.invoke()
    raw_box = None
    raw_score = None
    for detail in output_details:
        out_scale, out_zero_point = detail["quantization"]
        values = (interpreter.get_tensor(detail["index"]).astype(np.float32) - out_zero_point) * out_scale
        if values.shape[1] == 4:
            raw_box = values
        elif values.shape[1] == 1:
            raw_score = values
    return raw_box, raw_score


def postprocess(output_box: np.ndarray, output_score: np.ndarray, ratio: float,
                offset_x: int, offset_y: int, conf: float, iou: float, max_det: int):
    xyxy, scores = decode_logits(output_box, output_score)
    keep = scores > conf
    xyxy = xyxy[keep]
    scores = scores[keep]
    if not len(xyxy):
        return np.zeros((0, 4)), np.zeros(0)
    tlwh = np.column_stack([xyxy[:, 0], xyxy[:, 1], xyxy[:, 2] - xyxy[:, 0], xyxy[:, 3] - xyxy[:, 1]])
    indices = cv2.dnn.NMSBoxes(tlwh.tolist(), scores.astype(float).tolist(), conf, iou)
    kept = np.asarray(indices).reshape(-1)[:max_det] if len(indices) else []
    boxes = []
    box_scores = []
    for index in kept:
        x1, y1, x2, y2 = xyxy[int(index)]
        boxes.append([(x1 - offset_x) / ratio, (y1 - offset_y) / ratio, (x2 - offset_x) / ratio, (y2 - offset_y) / ratio])
        box_scores.append(float(scores[int(index)]))
    return np.asarray(boxes, dtype=np.float32).reshape(-1, 4), np.asarray(box_scores)


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
    """Lightweight IoU tracker that smooths boxes and bridges brief misses."""

    def __init__(self, iou_threshold: float = 0.35, max_miss: int = 3, confirm_frames: int = 2):
        self.iou_threshold = iou_threshold
        self.max_miss = max_miss
        self.confirm_frames = confirm_frames
        self.tracks: list[dict] = []

    def update(self, boxes: np.ndarray, scores: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
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
        out_boxes = np.asarray([track["box"] for track in kept])
        out_scores = np.asarray([track["score"] for track in kept])
        order = np.argsort(out_scores)[::-1]
        return out_boxes[order], out_scores[order]


def draw(display: np.ndarray, boxes: np.ndarray, scores: np.ndarray):
    for person_id, (box, score) in enumerate(zip(boxes.astype(int), scores), 1):
        x1, y1, x2, y2 = box
        center_x = (x1 + x2) // 2
        center_y = (y1 + y2) // 2
        cv2.rectangle(display, (x1, y1), (x2, y2), (0, 220, 0), 2)
        cv2.putText(display, f"person{person_id} ({center_x},{center_y})", (x1, max(20, y1 - 6)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 220, 0), 2, cv2.LINE_AA)
    return display


def main() -> None:
    parser = argparse.ArgumentParser(description="Test the w05 96 full-INT8 TFLite face detector.")
    parser.add_argument("--model", default=str(DEFAULT_MODEL))
    parser.add_argument("--source", default="0")
    parser.add_argument("--conf", type=float, default=0.45)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--max-det", type=int, default=20)
    parser.add_argument("--no-track", action="store_true", help="Disable frame-to-frame smoothing.")
    parser.add_argument("--track-iou", type=float, default=0.35)
    parser.add_argument("--track-miss", type=int, default=3)
    parser.add_argument("--track-confirm", type=int, default=2)
    args = parser.parse_args()

    interpreter = Interpreter(model_path=str(args.model), num_threads=4)
    interpreter.allocate_tensors()

    source = int(args.source) if args.source.isdigit() else args.source
    if isinstance(source, int):
        capture = open_camera(source)
        previous_time = time.perf_counter()
        tracker = TemporalTracker(args.track_iou, args.track_miss, args.track_confirm)
        print("Camera started. Press q or Esc to exit.")
        try:
            while True:
                ok, frame = capture.read()
                if not ok:
                    break
                canvas, ratio, offset_x, offset_y = preprocess_gray(frame)
                raw_box, raw_score = run_model(interpreter, canvas)
                boxes, scores = postprocess(raw_box, raw_score, ratio, offset_x, offset_y,
                                            args.conf, args.iou, args.max_det)
                if not args.no_track:
                    boxes, scores = tracker.update(boxes, scores)
                display = draw(frame.copy(), boxes, scores)
                fps = 1.0 / max(time.perf_counter() - previous_time, 1e-6)
                previous_time = time.perf_counter()
                cv2.putText(display, f"w05 96 INT8 TFLite | faces {len(boxes)} | {fps:.1f} FPS", (12, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 220, 0), 2, cv2.LINE_AA)
                cv2.imshow("RA8P1 w05 96 INT8 TFLite Face Detection", display)
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
    canvas, ratio, offset_x, offset_y = preprocess_gray(frame)
    raw_box, raw_score = run_model(interpreter, canvas)
    boxes, scores = postprocess(raw_box, raw_score, ratio, offset_x, offset_y,
                                args.conf, args.iou, args.max_det)
    display = draw(frame, boxes, scores)
    output_dir = SCRIPT_DIR.parent / "output"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / Path(source).name
    cv2.imwrite(str(output_path), display)
    print(f"{Path(source).name}: {len(boxes)} faces -> {output_path}")


if __name__ == "__main__":
    main()
