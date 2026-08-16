#!/usr/bin/env python3
"""Distill the kd3 teacher into the v3 grayscale student (width 0.75 + SPPF)."""

import argparse
import os
from pathlib import Path

import cv2
import torch
import torch.nn as nn

PROJECT_DIR = Path(__file__).resolve().parent
os.environ.setdefault("YOLO_CONFIG_DIR", str(PROJECT_DIR / "Ultralytics"))
os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

from ultralytics import YOLO
from ultralytics.data.dataset import YOLODataset
import ultralytics.engine.trainer as trainer_module
import ultralytics.engine.validator as validator_module
import ultralytics.engine.predictor as predictor_module
import ultralytics.engine.exporter as exporter_module
import ultralytics.utils.benchmarks as benchmarks_module
import ultralytics.utils.checks as checks_module

from lite_head import DistillTrainer

DEFAULT_TEACHER = (
    PROJECT_DIR.parent
    / "runs/detect/runs/detect/face_no_dfl_kd3/weights/best.pt"
)

_ORIGINAL_LOAD_IMAGE = YOLODataset.load_image


def teacher_to_gray(teacher) -> None:
    """Convert the teacher's stem to a 1-channel conv by averaging RGB weights."""
    conv_module = teacher.model.model[0]
    old_conv = conv_module.conv
    new_conv = nn.Conv2d(
        1,
        old_conv.out_channels,
        kernel_size=old_conv.kernel_size,
        stride=old_conv.stride,
        padding=old_conv.padding,
        bias=old_conv.bias is not None,
    )
    with torch.no_grad():
        new_conv.weight.data.copy_(old_conv.weight.data.mean(dim=1, keepdim=True))
        if old_conv.bias is not None:
            new_conv.bias.data.copy_(old_conv.bias.data)
    conv_module.conv = new_conv


def _gray_load_image(self, i: int, rect_mode: bool = True):
    image, image_file, shape = _ORIGINAL_LOAD_IMAGE(self, i, rect_mode)
    if image.ndim == 3 and image.shape[2] == 3:
        image = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)[..., None]
    elif image.ndim == 2:
        image = image[..., None]
    return image, image_file, shape


class GrayDistillTrainer(DistillTrainer):
    """Distillation trainer that builds a 1-channel student and grayscale loader."""

    def get_model(self, cfg=None, weights=None, verbose=True):
        self.data["channels"] = 1
        return super().get_model(cfg, weights, verbose)


def main() -> None:
    parser = argparse.ArgumentParser(description="Train the v3 grayscale lite-head face detector with distillation.")
    parser.add_argument("--model", default=str(PROJECT_DIR / "yolov8_face_mbv2_96_w075_sppf_no_dfl.yaml"))
    parser.add_argument("--teacher", default=str(DEFAULT_TEACHER))
    parser.add_argument("--data", default=str(PROJECT_DIR / "meeting_face_gray.yaml"))
    parser.add_argument("--lite-mid", type=int, default=32)
    parser.add_argument("--epochs", type=int, default=180)
    parser.add_argument("--imgsz", type=int, default=96)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--device", default="0" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--patience", type=int, default=50)
    parser.add_argument("--name", default="face_v3_w075_sppf_kd")
    parser.add_argument("--kd-box", type=float, default=5.0)
    parser.add_argument("--kd-cls", type=float, default=1.0)
    parser.add_argument("--kd-feat", type=float, default=3.0)
    parser.add_argument("--fraction", type=float, default=1.0)
    parser.add_argument("--no-val", action="store_true")
    parser.add_argument("--no-teacher", action="store_true")
    parser.add_argument(
        "--init-head",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Initialize the lite head from the teacher head channels.",
    )
    args = parser.parse_args()

    if args.imgsz % 32 != 0:
        original_check_imgsz = trainer_module.check_imgsz

        def allow_16_multiple(imgsz, stride=32, min_dim=1, max_dim=2, floor=0):
            result = original_check_imgsz(imgsz, stride=stride, min_dim=min_dim, max_dim=max_dim, floor=floor)
            if isinstance(imgsz, (list, tuple)) and len(imgsz) == 1 and isinstance(imgsz[0], int):
                candidate = imgsz[0]
                result_value = result[0] if isinstance(result, (list, tuple)) else result
                if candidate % 16 == 0 and candidate < result_value:
                    return type(imgsz)([candidate])
            elif isinstance(imgsz, int):
                result_value = result[0] if isinstance(result, (list, tuple)) else result
                if imgsz % 16 == 0 and imgsz < result_value:
                    return imgsz
            return result

        for module in (
            trainer_module,
            validator_module,
            predictor_module,
            exporter_module,
            benchmarks_module,
            checks_module,
        ):
            module.check_imgsz = allow_16_multiple
        print(f"Patched imgsz checks to allow {args.imgsz} (16-multiple).")

    YOLODataset.load_image = _gray_load_image

    GrayDistillTrainer.kd_teacher = None
    if not args.no_teacher:
        teacher = YOLO(args.teacher)
        teacher_to_gray(teacher)
        teacher.model.eval()
        for parameter in teacher.model.parameters():
            parameter.requires_grad_(False)
        GrayDistillTrainer.kd_teacher = teacher
    GrayDistillTrainer.lite_mid = args.lite_mid
    GrayDistillTrainer.kd_box = args.kd_box
    GrayDistillTrainer.kd_cls = args.kd_cls
    GrayDistillTrainer.kd_feat = args.kd_feat
    GrayDistillTrainer.kd_init_head = args.init_head

    print(
        f"v3 grayscale distill | teacher: {args.teacher} | mid: {args.lite_mid} "
        f"| init-from-teacher: {args.init_head} | kd weights: box={args.kd_box}, cls={args.kd_cls}, feat={args.kd_feat}"
    )

    student = YOLO(args.model)
    student.train(
        data=args.data,
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        device=args.device,
        project="runs/detect",
        name=args.name,
        pretrained=True,
        single_cls=True,
        workers=0,
        patience=args.patience,
        val=not args.no_val,
        fraction=args.fraction,
        trainer=GrayDistillTrainer,
        hsv_h=0.0,
        hsv_s=0.0,
        hsv_v=0.0,
        translate=0.1,
        scale=0.5,
        fliplr=0.5,
    )


if __name__ == "__main__":
    main()
