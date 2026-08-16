#!/usr/bin/env python3
"""PTQ-calibrate the packaged w05 96 ONNX to INT8 QDQ.

The detect-head decode ops (softplus/sigmoid/concat) stay in FP32 so the
concatenated box/score output does not collapse under INT8 activation ranges.
"""

import argparse
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
DEFAULT_ONNX = MODEL_DIR / "face.onnx"
DEFAULT_OUTPUT = MODEL_DIR / "face_int8.onnx"
os.environ.setdefault("YOLO_CONFIG_DIR", str(PROJECT_DIR / "Ultralytics"))
sys.path.insert(0, str(PROJECT_DIR))

from train_yolo_fastest import collect_paths, load_sample


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
    for image_path, _ in collect_paths("train"):
        canvas, _ = load_sample(image_path, image_path.with_suffix(".txt"), False, input_size=input_size)
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
    parser = argparse.ArgumentParser(description="Quantize w05 96 ONNX to INT8 QDQ.")
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
