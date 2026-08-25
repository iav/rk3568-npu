#!/usr/bin/env python
# Smoke test for float-input models (yolo exports): delegate, one invoke, time.
import os, sys, time, numpy as np
try:
    import tflite_runtime.interpreter as tflite
except ImportError:
    from ai_edge_litert import interpreter as tflite
m = sys.argv[1]
d = [tflite.load_delegate(os.environ.get("TEFLON_PATH", "./mesa/build/src/gallium/targets/teflon/libteflon.so"))]
ip = tflite.Interpreter(model_path=m, experimental_delegates=d)
ip.allocate_tensors()
i0 = ip.get_input_details()[0]
x = np.random.default_rng(1).random(i0["shape"]).astype(i0["dtype"])
ip.set_tensor(i0["index"], x)
t = time.time(); ip.invoke(); print("npu-invoke", round((time.time() - t) * 1000), "ms", flush=True)
