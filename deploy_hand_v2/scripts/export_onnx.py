#!/usr/bin/env python3
"""Export the v3 96 grayscale gesture detector to a fixed-shape ONNX.

Manual torch.onnx.export keeps the softplus box head so the ONNX matches the
PyTorch checkpoint exactly.
"""

import argparse
import os
import sys
from pathlib import Path

import torch

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(PROJECT_DIR))
MODEL_DIR = SCRIPT_DIR.parent / "model"
DEFAULT_MODEL = MODEL_DIR / "gesture_v3.pt"
os.environ.setdefault("YOLO_CONFIG_DIR", str(PROJECT_DIR / "Ultralytics"))

from ultralytics import YOLO


class _ExportWrapper(torch.nn.Module):
    def __init__(self, wrapped):
        super().__init__()
        self.wrapped = wrapped

    def forward(self, image):
        output, _ = self.wrapped(image)
        return output


def main() -> None:
    parser = argparse.ArgumentParser(description="Export v1 gesture model to ONNX.")
    parser.add_argument("--weights", default=str(DEFAULT_MODEL))
    parser.add_argument("--imgsz", type=int, default=96)
    parser.add_argument("--opset", type=int, default=17)
    args = parser.parse_args()

    model = YOLO(args.weights)
    channels = model.model.model[0].conv.weight.shape[1]
    output = MODEL_DIR / "gesture.onnx"
    wrapper = _ExportWrapper(model.model).eval()
    torch.onnx.export(
        wrapper,
        torch.zeros(1, channels, args.imgsz, args.imgsz),
        str(output),
        opset_version=args.opset,
        input_names=["images"],
        output_names=["output0"],
    )
    print(f"ONNX saved to {output}")


if __name__ == "__main__":
    main()
