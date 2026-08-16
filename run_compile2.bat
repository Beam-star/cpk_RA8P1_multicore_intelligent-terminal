@echo off
cd /d E:\ruhmi-framework-mcu-main\scripts
E:\ruhmi-framework-mcu-main\.venv\Scripts\python.exe mcu_compile.py E:\RA8P1\Titan-mini_RA8P1_multicore\Titan-mini_RA8P1_multicore_CPU0\src\yolov5\yolov5n.tflite E:\RA8P1\Titan-mini_RA8P1_multicore\Titan-mini_RA8P1_multicore_CPU0\src\yolov5\deploy_output --npu --quantize --external
echo EXIT_CODE=%ERRORLEVEL%
