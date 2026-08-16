#!/usr/bin/env python3
"""PTQ-calibrate the v1 gesture ONNX to INT8 QDQ.

Head decode ops (softplus/sigmoid/concat) stay in FP32 so the box/score output
does not collapse under INT8 activation ranges.
"""

import argparse
import cv2
import os
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parents[1]
MODEL_DIR = SCRIPT_DIR.parent / "model"
DATASET = PROJECT_DIR.parent.parent / "datasets" / "meeting_palm_stop_11k_mix"
DEFAULT_ONNX = MODEL_DIR / "gesture.onnx"
DEFAULT_OUTPUT = MODEL_DIR / "gesture_int8.onnx"
os.environ.setdefault("YOLO_CONFIG_DIR", str(PROJECT_DIR / "Ultralytics"))

CLASS_NAMES = [
    "hand",
]


class GrayCalibrationReader(CalibrationDataReader):
    def __init__(self, input_name, images):
        self.input_name = input_name
        self.images = images
        self.index = 0

    def get_next(self):
        if self.index >= len(self.images):
            return None
        image = self.images[self.index]
        self.index += 1
        return {self.input_name: (image.astype(np.float32) / 255.0)[None, None]}


def collect_calibration_images(count=200, input_size=96):
    images = []
    image_root = DATASET / "images" / "train"
    for image_path in sorted(image_root.glob("*.jpg")):
        image = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
        if image is None:
            continue
        height, width = image.shape[:2]
        ratio = min(input_size / height, input_size / width)
        new_width = max(1, round(width * ratio))
        new_height = max(1, round(height * ratio))
        resized = cv2.resize(image, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
        canvas = np.full((input_size, input_size), 114, dtype=np.uint8)
        offset_x = (input_size - new_width) // 2
        offset_y = (input_size - new_height) // 2
        canvas[offset_y:offset_y + new_height, offset_x:offset_x + new_width] = resized
        images.append(canvas)
        if len(images) >= count:
            break
    return images


def quantize_onnx_int8(model_path, output_path, calibration_images):
    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    input_name = session.get_inputs()[0].name
    reader = GrayCalibrationReader(input_name, calibration_images)
    graph = onnx.load(str(model_path))
    head_prefix = ""
    for node in graph.graph.node:
        if node.op_type == "Concat" and "output0" in node.output:
            head_prefix = node.name.rsplit("/", 1)[0] + "/"
            break
    nodes_to_exclude = [
        node.name
        for node in graph.graph.node
        if head_prefix and node.name.startswith(head_prefix)
        and node.op_type in {"Sigmoid", "Mul", "Concat", "Softplus"}
    ]
    quantize_static(
        model_input=str(model_path),
        model_output=str(output_path),
        calibration_data_reader=reader,
        quant_format=QuantFormat.QDQ,
        activation_type=QuantType.QUInt8,
        weight_type=QuantType.QInt8,
        per_channel=True,
        nodes_to_exclude=nodes_to_exclude,
    )
    quantized = onnx.load(str(output_path))
    del quantized.metadata_props[:]
    onnx.save(quantized, str(output_path))
    return output_path


def main() -> None:
    parser = argparse.ArgumentParser(description="Quantize v1 gesture ONNX to INT8 QDQ.")
    parser.add_argument("--model", default=str(DEFAULT_ONNX))
    parser.add_argument("--output", default=str(DEFAULT_OUTPUT))
    parser.add_argument("--calib", type=int, default=200)
    parser.add_argument("--calib-size", type=int, default=96)
    args = parser.parse_args()

    calibration_images = collect_calibration_images(args.calib, args.calib_size)
    print(f"Quantizing with {len(calibration_images)} calibration images -> {args.output}")
    quantize_onnx_int8(args.model, args.output, calibration_images)
    print("INT8 ONNX saved.")


if __name__ == "__main__":
    main()


