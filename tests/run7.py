#!/usr/bin/env python
# multi-input oracle in ONE process: fills 0,64,128,192,255 + row gradient
import sys, numpy as np
import tflite_runtime.interpreter as tflite

model, mode = sys.argv[1], sys.argv[2]
d = [tflite.load_delegate("/media/nvme/mesa/build/src/gallium/targets/teflon/libteflon.so")] if mode == "npu" else []
ip = tflite.Interpreter(model_path=model, experimental_delegates=d)
ip.allocate_tensors()
inp = ip.get_input_details()[0]; out = ip.get_output_details()[0]
h, w, c = inp["shape"][1], inp["shape"][2], inp["shape"][3]
res = {}
for name, fill in [("f0",0),("f64",64),("f128",128),("f192",192),("f255",255)]:
    x = np.full((1,h,w,c), fill, dtype=np.uint8)
    ip.set_tensor(inp["index"], x); ip.invoke()
    res[name] = ip.get_tensor(out["index"])[0].copy()
x = np.zeros((1,h,w,c), dtype=np.uint8)
for r in range(h): x[0,r,:,:] = 120 + (r % 8)   # small around zp=128
ip.set_tensor(inp["index"], x); ip.invoke()
res["grad"] = ip.get_tensor(out["index"])[0].copy()
np.savez(sys.argv[3], **res)
print("ok")
