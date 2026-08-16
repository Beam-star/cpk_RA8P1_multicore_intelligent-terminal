"""
Verify the INT8 model against the FP32 baseline.

The embedding model output goes through MERA INT8 quantization on the NPU,
then we dequantize on CPU with a GUESSED scale (1/255).  If the guess
is wrong, cosine similarity between same-person embeddings is degraded.

This script:
  1. Runs the FP32 model on 10 random inputs
  2. Simulates INT8 symmetric quant/dequant with the guessed scale
  3. Compares cosine similarity between FP32 and simulated-INT8 outputs

If avg cosine similarity < 0.95, the dequant scale is WRONG.
"""
import tensorflow as tf
import numpy as np
import os

MODEL_PATH = 'mobilefacenet.tflite'
OUT_SCALE   = 0.003921568859368563   # current guess (1/255)
OUT_ZP      = 0
EMBED_DIM   = 128

print(f"Loading model: {MODEL_PATH}")
interp = tf.lite.Interpreter(model_path=MODEL_PATH)
interp.allocate_tensors()
inp_idx  = interp.get_input_details()[0]['index']
out_idx  = interp.get_output_details()[0]['index']
inp_shape = interp.get_input_details()[0]['shape']
print(f"  Input:  {inp_shape}")
print(f"  Output: {interp.get_output_details()[0]['shape']}")

def simulate_int8(fp32_val, scale, zp):
    """Simulate MERA INT8 symmetric quantize → dequantize."""
    q = np.round(fp32_val / scale) + zp
    q = np.clip(q, -128, 127)
    return (q.astype(np.float32) - zp) * scale

def cosine(a, b):
    a_n = a / (np.linalg.norm(a) + 1e-8)
    b_n = b / (np.linalg.norm(b) + 1e-8)
    return np.dot(a_n, b_n)

# Test on 10 random inputs (simulating different face crops)
cosines = []
print("\nTesting 10 random inputs...")
for i in range(10):
    inp = np.random.randn(*inp_shape).astype(np.float32)

    # FP32 inference
    interp.set_tensor(inp_idx, inp)
    interp.invoke()
    fp32_out = interp.get_tensor(out_idx)[0]

    # Simulate INT8 quant/dequant with current scale
    int8_out = simulate_int8(fp32_out, OUT_SCALE, OUT_ZP)

    sim = cosine(fp32_out, int8_out)
    cosines.append(sim)
    print(f"  [{i}] FP32 vs simulated-INT8 cosine: {sim:.4f}")

avg = np.mean(cosines)
print(f"\nAverage cosine similarity: {avg:.4f}")

if avg >= 0.95:
    print("✓ Dequant scale is CORRECT — quantization is not the accuracy bottleneck.")
    print("  Problem is the ImageNet-pretrained features. Need a face-trained model.")
else:
    print("✗ Dequant scale is WRONG — this IS hurting accuracy significantly.")
    # Try to find the right scale
    print("\nSearching for correct output scale...")
    best_scale = OUT_SCALE
    best_avg = avg
    for s in np.logspace(-4, -0.5, 50):
        cos_list = []
        for i in range(10):
            inp = np.random.randn(*inp_shape).astype(np.float32)
            interp.set_tensor(inp_idx, inp)
            interp.invoke()
            fp32_out = interp.get_tensor(out_idx)[0]
            int8_out = simulate_int8(fp32_out, s, 0)
            cos_list.append(cosine(fp32_out, int8_out))
        m = np.mean(cos_list)
        if m > best_avg:
            best_avg = m
            best_scale = s
    print(f"  Best scale: {best_scale:.8f}  (cosine={best_avg:.4f})")
    print(f"  Update OUT_SCALE in face_embedding_task.c to this value.")
