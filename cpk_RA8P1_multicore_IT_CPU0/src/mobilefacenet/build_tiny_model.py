"""
Build a tiny face-embedding model based on MobileNetV2 0.35x with
ImageNet pre-trained backbone.  The pre-trained features transfer
surprisingly well to face similarity (a common trick in resource-
constrained embedded face recognition).

Output: tiny_face_embed.tflite (~1.6 MB FP32 → ~400 KB INT8 after RUHMI)
"""
import tensorflow as tf
import numpy as np
import os

INPUT_SIZE  = 96          # slightly smaller than 112 → faster, more compact
EMBED_DIM   = 128         # compact embedding

print(f"Building MobileNetV2 α=0.35  input={INPUT_SIZE}×{INPUT_SIZE}×3  emb={EMBED_DIM}d")

# ---- Backbone (ImageNet pre-trained, frozen) ----
backbone = tf.keras.applications.MobileNetV2(
    input_shape=(INPUT_SIZE, INPUT_SIZE, 3),
    alpha=0.35,
    include_top=False,
    weights='imagenet',
    pooling='avg',               # GlobalAveragePooling2D built-in
)
backbone.trainable = False
print(f"  Backbone parameters: {backbone.count_params():,}")

# ---- Embedding head ----
inputs = tf.keras.layers.Input(shape=(INPUT_SIZE, INPUT_SIZE, 3), name='input')
x = backbone(inputs, training=False)

# GlobalAveragePooling2D output is (batch, 1280).  Use Conv2D 1×1
# instead of Dense to avoid FULLY_CONNECTED v12 (MER incompatible).
x = tf.keras.layers.Reshape((1, 1, 1280))(x)
x = tf.keras.layers.Conv2D(EMBED_DIM, 1, use_bias=False, name='pre_embedding')(x)
x = tf.keras.layers.Flatten()(x)

# NO Lambda/L2-normalize here — MERA can't handle custom TF ops.
# L2-normalisation is done on CPU after NPU inference.

model = tf.keras.Model(inputs, x)
print(f"  Total parameters:    {model.count_params():,}")
print(f"  Trainable:           {model.trainable_weights}")

# ---- Quick forward-pass test ----
test_in = np.random.randn(1, INPUT_SIZE, INPUT_SIZE, 3).astype(np.float32)
out = model(test_in, training=False)
print(f"  Output shape:        {out.shape}")
print(f"  Output norm:         {np.linalg.norm(out):.4f}")

# ---- Ensure ALL weights are FP32 (prevent MERA Int8VecConstant errors) ----
for w in model.weights:
    w.assign(tf.cast(w, tf.float32))

# ---- Convert to TFLite Float32 ----
converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS]
converter.allow_custom_ops = False
tflite_fp32 = converter.convert()

out_dir = os.path.dirname(os.path.abspath(__file__))
tflite_path = os.path.join(out_dir, 'tiny_face_embed.tflite')
with open(tflite_path, 'wb') as f:
    f.write(tflite_fp32)

size_kb = os.path.getsize(tflite_path) / 1024
print(f"\n  TFLite FP32: {os.path.getsize(tflite_path):,} bytes ({size_kb:.1f} KB)")

# ---- Verify TFLite ----
interp = tf.lite.Interpreter(model_path=tflite_path)
interp.allocate_tensors()
inp = interp.get_input_details()[0]
out_d = interp.get_output_details()[0]
print(f"  TFLite input:  {inp['name']}  shape={inp['shape']}  dtype={inp['dtype']}")
print(f"  TFLite output: {out_d['name']}  shape={out_d['shape']}  dtype={out_d['dtype']}")

# Quick inference test on random data
interp.set_tensor(inp['index'], test_in)
interp.invoke()
res = interp.get_tensor(out_d['index'])
print(f"  TFLite output norm: {np.linalg.norm(res):.4f}")
print("\nDone. Copy tiny_face_embed.tflite → mobilefacenet.tflite and run run_compile_mfn.bat")
